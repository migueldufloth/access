"""API REST do Access (R4): estado, histórico, comandos, analítico e agendamentos. Também serve a interface (R6)."""

import logging
import re
import threading
from contextlib import asynccontextmanager
from datetime import datetime, time, timedelta, timezone
from pathlib import Path

from fastapi import Depends, FastAPI, HTTPException, Query, Request
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from . import analitico, auth
from .config import Config
from .contrato import ErroContrato
from .db import Banco
from .mqtt import ClienteMqtt
from .notificacao import Ntfy
from .servico import Servico

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
log = logging.getLogger("access")
WEB = Path(__file__).resolve().parent / "web"
INTERVALO_CICLO_S = 2


class LoginIn(BaseModel):
    usuario: str
    senha: str


class ComandoIn(BaseModel):
    deviceId: str
    acao: str
    params: dict = {}
    expira_s: int = 30


class AgendamentoIn(BaseModel):
    device_id: str
    acao: str = Field(pattern="^(armar|desarmar)$")
    hora: time
    dias_semana: list[int] = Field(min_length=1)


class ConfiguracaoIn(BaseModel):
    limiteEscalonamento_s: int = Field(ge=10, le=600)


def create_app(config: Config | None = None, publicar=None, notificador=None, ciclo_automatico: bool = True) -> FastAPI:
    """`publicar` e `notificador` podem ser trocados nos testes; por padrão usam o broker e o ntfy reais."""

    @asynccontextmanager
    async def ciclo_de_vida(app: FastAPI):
        cfg = config or Config.do_ambiente()
        banco = Banco(cfg.database_url)
        banco.migrar()
        for nome, senha, perfil in ((cfg.admin_usuario, cfg.admin_senha, "administrador"),
                                    (cfg.operador_usuario, cfg.operador_senha, "operador")):
            banco.executar("INSERT INTO usuarios (nome_usuario, senha_hash, perfil) VALUES (%s, %s, %s) "
                           "ON CONFLICT (nome_usuario) DO UPDATE SET senha_hash = EXCLUDED.senha_hash, "
                           "perfil = EXCLUDED.perfil", (nome, auth.gerar_hash(senha), perfil))

        cliente = None
        servico = Servico(banco, publicar, notificador or Ntfy(cfg.ntfy_url, cfg.ntfy_topico), cfg.fuso)
        if publicar is None:
            cliente = ClienteMqtt(cfg.mqtt_host, cfg.mqtt_porta, cfg.mqtt_usuario, cfg.mqtt_senha, servico.ingerir)
            servico.publicar = cliente.publicar
            cliente.iniciar()

        parar = threading.Event()

        def laco():
            while not parar.wait(INTERVALO_CICLO_S):
                try:
                    servico.ciclo()
                except Exception:
                    log.exception("erro no ciclo de regras")

        if ciclo_automatico:
            threading.Thread(target=laco, name="ciclo-regras", daemon=True).start()

        app.state.config, app.state.banco, app.state.servico = cfg, banco, servico
        yield
        parar.set()
        if cliente:
            cliente.parar()
        banco.fechar()

    app = FastAPI(title="Access API", version="1.0.0", lifespan=ciclo_de_vida)

    @app.exception_handler(ErroContrato)
    async def _erro_contrato(_req: Request, e: ErroContrato):
        return JSONResponse(status_code=422, content={"erro": e.codigo, "motivo": e.motivo})

    def banco(request: Request) -> Banco:
        return request.app.state.banco

    def servico(request: Request) -> Servico:
        return request.app.state.servico

    @app.get("/api/saude")
    def saude(b: Banco = Depends(banco)):
        b.um("SELECT 1")
        return {"ok": True}

    @app.post("/api/auth/login")
    def login(dados: LoginIn, request: Request, b: Banco = Depends(banco)):
        usuario = b.um("SELECT * FROM usuarios WHERE nome_usuario = %s", (dados.usuario,))
        if not usuario or not auth.senha_confere(dados.senha, usuario["senha_hash"]):
            raise HTTPException(401, "usuário ou senha inválidos")
        return {"token": auth.emitir_token(usuario, request.app.state.config.jwt_secret),
                "nome": usuario["nome_usuario"], "perfil": usuario["perfil"]}

    @app.get("/api/eu")
    def eu(usuario: dict = Depends(auth.usuario_atual)):
        return usuario

    @app.get("/api/dispositivos", dependencies=[Depends(auth.usuario_atual)])
    def dispositivos(b: Banco = Depends(banco)):
        return b.todos(
            "SELECT d.*, (SELECT json_agg(json_build_object('grandeza', m.grandeza, 'valor', m.valor, "
            "                 'unidade', m.unidade, 'ts', m.ts)) "
            "             FROM (SELECT DISTINCT ON (grandeza) * FROM medicoes "
            "                   WHERE device_id = d.device_id AND grandeza <> 'alarme' "
            "                   ORDER BY grandeza, ts DESC, id DESC) m) AS ultimas "
            "FROM dispositivos d ORDER BY d.nome")

    @app.get("/api/dispositivos/{device_id}/medicoes", dependencies=[Depends(auth.usuario_atual)])
    def medicoes(device_id: str, grandeza: str | None = None, de: datetime | None = None,
                 ate: datetime | None = None, limite: int = Query(500, ge=1, le=5000), b: Banco = Depends(banco)):
        return b.todos(
            "SELECT * FROM (SELECT grandeza, valor, unidade, ts FROM medicoes WHERE device_id = %s "
            "  AND (%s::text IS NULL OR grandeza = %s) AND (%s::timestamptz IS NULL OR ts >= %s) "
            "  AND (%s::timestamptz IS NULL OR ts <= %s) ORDER BY ts DESC, id DESC LIMIT %s) m ORDER BY ts",
            (device_id, grandeza, grandeza, de, de, ate, ate, limite))

    @app.get("/api/alertas", dependencies=[Depends(auth.usuario_atual)])
    def alertas(ativos: bool = False, b: Banco = Depends(banco)):
        return b.todos("SELECT * FROM alertas WHERE NOT %s OR encerrado_em IS NULL ORDER BY aberto_em DESC LIMIT 100",
                       (ativos,))

    @app.get("/api/comandos", dependencies=[Depends(auth.usuario_atual)])
    def comandos(limite: int = Query(30, ge=1, le=500), b: Banco = Depends(banco)):
        return b.todos("SELECT c.*, u.nome_usuario FROM comandos c LEFT JOIN usuarios u ON u.id = c.usuario_id "
                       "ORDER BY enviado_em DESC LIMIT %s", (limite,))

    @app.get("/api/comandos/{command_id}", dependencies=[Depends(auth.usuario_atual)])
    def comando(command_id: str, s: Servico = Depends(servico)):
        if not re.fullmatch(r"[0-9a-f-]{36}", command_id) or not (cmd := s.comando(command_id)):
            raise HTTPException(404, "comando não encontrado")
        return cmd

    @app.post("/api/comandos", status_code=202)
    def enviar_comando(dados: ComandoIn, usuario: dict = Depends(auth.exigir_admin), s: Servico = Depends(servico)):
        try:
            return s.enviar_comando(dados.deviceId, dados.acao, dados.params, dados.expira_s, "usuario", usuario["id"])
        except LookupError as e:
            raise HTTPException(404, str(e)) from None

    @app.get("/api/analitico/{device_id}", dependencies=[Depends(auth.usuario_atual)])
    def analitico_(device_id: str, horas: int = Query(24, ge=1, le=168), b: Banco = Depends(banco)):
        fim = datetime.now(timezone.utc)
        inicio = fim - timedelta(hours=horas)

        def serie(grandezas: list[str]) -> tuple[list, int]:
            linhas = b.todos("SELECT ts, valor FROM medicoes WHERE device_id = %s AND grandeza = ANY(%s) "
                             "AND ts >= %s ORDER BY ts, id", (device_id, grandezas, inicio))
            antes = b.um("SELECT valor FROM medicoes WHERE device_id = %s AND grandeza = ANY(%s) AND ts < %s "
                         "ORDER BY ts DESC, id DESC LIMIT 1", (device_id, grandezas, inicio))
            return [(r["ts"], int(r["valor"])) for r in linhas], int(antes["valor"]) if antes else 0

        sensor, sensor_antes = serie(["abertura", "presenca"])
        alarme, alarme_antes = serie(["alarme"])
        status = b.todos("SELECT ts, status FROM eventos_status WHERE device_id = %s AND ts >= %s ORDER BY ts, id",
                         (device_id, inicio))
        status_antes = b.um("SELECT status FROM eventos_status WHERE device_id = %s AND ts < %s "
                            "ORDER BY ts DESC, id DESC LIMIT 1", (device_id, inicio))
        rssi = b.todos("SELECT ts, valor FROM (SELECT ts, valor, id FROM medicoes WHERE device_id = %s "
                       "AND grandeza = 'rssi' ORDER BY ts DESC, id DESC LIMIT 120) r ORDER BY ts, id", (device_id,))
        # Dispositivo mais novo que a janela: o uptime conta a partir do primeiro contato, não de 24 h atrás.
        desde = max(inicio, status[0]["ts"]) if status and not status_antes else inicio
        online_s = analitico.tempo_ativo_s([(r["ts"], r["status"] == "online") for r in status], desde, fim,
                                           bool(status_antes and status_antes["status"] == "online"))
        return {
            "horas": horas,
            "eventos_por_hora": analitico.eventos_por_hora(sensor, fim, horas, sensor_antes),
            "tempo_em_alarme_s": round(analitico.tempo_ativo_s(
                [(t, v == 1) for t, v in alarme], inicio, fim, alarme_antes == 1)),
            "uptime_pct": round(100 * online_s / max((fim - desde).total_seconds(), 1), 1),
            "rssi": analitico.media_movel([(r["ts"], r["valor"]) for r in rssi]),
        }

    @app.get("/api/agendamentos", dependencies=[Depends(auth.usuario_atual)])
    def agendamentos(b: Banco = Depends(banco)):
        return b.todos("SELECT * FROM agendamentos ORDER BY device_id, hora")

    @app.post("/api/agendamentos", status_code=201, dependencies=[Depends(auth.exigir_admin)])
    def criar_agendamento(dados: AgendamentoIn, b: Banco = Depends(banco)):
        if any(d not in range(7) for d in dados.dias_semana):
            raise HTTPException(422, "dias_semana vai de 0 (segunda) a 6 (domingo)")
        if not b.um("SELECT 1 FROM dispositivos WHERE device_id = %s", (dados.device_id,)):
            raise HTTPException(404, "dispositivo não cadastrado")
        return b.um("INSERT INTO agendamentos (device_id, acao, hora, dias_semana) VALUES (%s, %s, %s, %s) "
                    "RETURNING *", (dados.device_id, dados.acao, dados.hora.replace(second=0, microsecond=0),
                                    sorted(set(dados.dias_semana))))

    @app.delete("/api/agendamentos/{ag_id}", status_code=204, dependencies=[Depends(auth.exigir_admin)])
    def remover_agendamento(ag_id: int, b: Banco = Depends(banco)):
        if not b.executar("DELETE FROM agendamentos WHERE id = %s", (ag_id,)):
            raise HTTPException(404, "agendamento não encontrado")

    @app.get("/api/configuracao", dependencies=[Depends(auth.usuario_atual)])
    def configuracao(b: Banco = Depends(banco)):
        return {r["chave"]: r["valor"] for r in b.todos("SELECT * FROM configuracoes")}

    @app.put("/api/configuracao", dependencies=[Depends(auth.exigir_admin)])
    def alterar_configuracao(dados: ConfiguracaoIn, s: Servico = Depends(servico)):
        s.configurar_limite(dados.limiteEscalonamento_s)
        return dados

    @app.get("/api/mensagens-invalidas", dependencies=[Depends(auth.usuario_atual)])
    def mensagens_invalidas(limite: int = Query(50, ge=1, le=500), b: Banco = Depends(banco)):
        return b.todos("SELECT * FROM mensagens_invalidas ORDER BY recebido_em DESC LIMIT %s", (limite,))

    @app.get("/firmware/{arquivo}")
    def firmware(arquivo: str, request: Request):
        """Binário do OTA (R17). Público: não tem segredo e o dispositivo confere o SHA-256 enviado no comando."""
        caminho = request.app.state.config.firmware_dir / arquivo
        if not re.fullmatch(r"[\w.-]+\.bin", arquivo) or not caminho.is_file():
            raise HTTPException(404, "firmware não encontrado")
        return FileResponse(caminho, media_type="application/octet-stream")

    app.mount("/", StaticFiles(directory=WEB, html=True), name="web")
    return app
