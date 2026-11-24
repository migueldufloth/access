"""Regras de negócio do backend: ingestão validada (R4/R5), ciclo de comando (R7), alertas e decisão remota (R8)."""

import logging
import uuid
from datetime import datetime, timedelta, timezone
from typing import Callable
from zoneinfo import ZoneInfo

from psycopg.types.json import Jsonb

from . import regras
from .contrato import PREFIXO, ErroContrato, montar_comando, separar_topico, validar_comando, validar_mensagem
from .db import Banco
from .mqtt import BrokerIndisponivel

log = logging.getLogger(__name__)

_COLUNA_DO_EVENTO = {"recebido": "recebido_em", "rejeitado": "recebido_em", "executado": "executado_em",
                     "falha": "executado_em", "confirmado": "confirmado_em"}


def _agora() -> datetime:
    return datetime.now(timezone.utc)


class Servico:
    def __init__(self, banco: Banco, publicar: Callable[[str, bytes], None], notificar, fuso: str):
        self.banco, self.publicar, self.notificar = banco, publicar, notificar
        self.fuso = ZoneInfo(fuso)

    # --- Ingestão (dispositivo -> backend) ---------------------------------

    def ingerir(self, topico: str, bruto: bytes, agora: datetime | None = None) -> None:
        agora = agora or _agora()
        partes = separar_topico(topico)
        if partes is None:
            return self._invalida(topico, bruto, "tópico fora da árvore do contrato", agora)
        device_id, tipo = partes
        try:
            msg = validar_mensagem(tipo, device_id, bruto)
        except ErroContrato as e:
            return self._invalida(topico, bruto, f"{e.codigo}: {e.motivo}", agora)

        ts = msg["_ts"] or agora   # ts null: vale a hora de recebimento (seção 1 do contrato)
        if tipo == "status" and msg["status"] == "offline":
            return self._mudar_status(device_id, False, agora)
        self._tocar(device_id, agora, msg.get("fw") if tipo == "status" else None)
        if tipo == "status":
            return
        if tipo == "telemetria":
            self._telemetria(device_id, msg, ts, agora)
        elif tipo == "estado":
            self._estado(device_id, msg, ts, agora)
        else:
            self._confirmacao(device_id, msg, agora)

    def _invalida(self, topico: str, bruto: bytes, motivo: str, agora: datetime) -> None:
        log.warning("mensagem inválida em %s: %s", topico, motivo)
        self.banco.executar("INSERT INTO mensagens_invalidas (topico, payload, motivo, recebido_em) "
                            "VALUES (%s, %s, %s, %s)",
                            (topico, bruto[:2048].decode("utf-8", "replace"), motivo, agora))

    def _tocar(self, device_id: str, agora: datetime, fw: str | None = None) -> None:
        """Mensagem válida do dispositivo: registra o contato, marca online e encerra o alerta de offline
        (que também abre por silêncio, sem o status ter mudado)."""
        self._mudar_status(device_id, True, agora)
        self.banco.executar("UPDATE dispositivos SET ultimo_contato = %s, fw_versao = COALESCE(%s, fw_versao) "
                            "WHERE device_id = %s", (agora, fw, device_id))
        self.fechar_alerta(device_id, "dispositivo_offline", agora)

    def _mudar_status(self, device_id: str, online: bool, agora: datetime) -> None:
        """Atualiza online/offline e registra o evento só quando muda (base da disponibilidade)."""
        self.banco.executar("INSERT INTO dispositivos (device_id, nome) VALUES (%s, %s) ON CONFLICT DO NOTHING",
                            (device_id, device_id))
        if self.banco.um("UPDATE dispositivos SET online = %s WHERE device_id = %s AND online IS DISTINCT FROM %s "
                         "RETURNING device_id", (online, device_id, online)):
            self.banco.executar("INSERT INTO eventos_status (device_id, status, ts) VALUES (%s, %s, %s)",
                                (device_id, "online" if online else "offline", agora))
            log.info("%s ficou %s", device_id, "online" if online else "offline")

    def _telemetria(self, device_id: str, msg: dict, ts: datetime, agora: datetime) -> None:
        with self.banco.pool.connection() as c:
            c.cursor().executemany(
                "INSERT INTO medicoes (device_id, grandeza, valor, unidade, ts, recebido_em) "
                "VALUES (%s, %s, %s, %s, %s, %s)",
                [(device_id, m["grandeza"], m["valor"], m["unidade"], ts, agora) for m in msg["medicoes"]])
        abertura = next((m["valor"] for m in msg["medicoes"] if m["grandeza"] == "abertura"), None)
        if abertura == 1:
            self.banco.executar("UPDATE dispositivos SET aberta_desde = COALESCE(aberta_desde, %s) "
                                "WHERE device_id = %s", (ts, device_id))
        elif abertura == 0:
            self.banco.executar("UPDATE dispositivos SET aberta_desde = NULL WHERE device_id = %s", (device_id,))
            self.fechar_alerta(device_id, "abertura_armado", agora)

    def _estado(self, device_id: str, msg: dict, ts: datetime, agora: datetime) -> None:
        estado = {k: v for k, v in msg.items() if not k.startswith("_")}
        anterior = self.banco.um("SELECT estado FROM dispositivos WHERE device_id = %s", (device_id,))["estado"]
        self.banco.executar("UPDATE dispositivos SET estado = %s, fw_versao = %s WHERE device_id = %s",
                            (Jsonb(estado), msg["fw"], device_id))
        # Histórico do alarme como medição derivada: base do "tempo em alarme" do painel analítico.
        if anterior is None or anterior.get("alarme") != msg["alarme"]:
            self.banco.executar("INSERT INTO medicoes (device_id, grandeza, valor, unidade, ts, recebido_em) "
                                "VALUES (%s, 'alarme', %s, 'bool', %s, %s)",
                                (device_id, int(msg["alarme"] == "disparado"), ts, agora))
        if not msg["armado"]:   # desarmar resolve a invasão: o alerta do episódio se encerra
            self.fechar_alerta(device_id, "abertura_armado", agora)
        if msg["sensor"].startswith("falha"):
            self.abrir_alerta(device_id, "falha_sensor", f"sensor em {msg['sensor']}", agora,
                              f"Falha de sensor em {device_id}", f"Sensor reportou {msg['sensor']}; modo {msg['modo']}.")
        elif msg["sensor"] == "ok":
            self.fechar_alerta(device_id, "falha_sensor", agora)
        if msg["ultimoComandoId"]:
            self._avancar(msg["ultimoComandoId"], device_id, "confirmado", agora)

    def _confirmacao(self, device_id: str, msg: dict, agora: datetime) -> None:
        evento = regras.evento_da_confirmacao(msg["fase"], msg["resultado"])
        cmd = self._avancar(msg["commandId"], device_id, evento, agora, msg["erro"], msg["motivo"])
        if cmd and evento == "rejeitado" and cmd["origem"] == "agendamento":
            self.abrir_alerta(device_id, "agendamento_nao_aplicado", f"{cmd['acao']}: {msg['erro']}", agora,
                              f"Agendamento não aplicado em {device_id}",
                              f"'{cmd['acao']}' rejeitado: {msg['motivo'] or msg['erro']}.")

    # --- Comandos (backend -> dispositivo) ---------------------------------

    def enviar_comando(self, device_id: str, acao: str, params: dict, expira_s: int, origem: str,
                       usuario_id: int | None = None, agora: datetime | None = None) -> dict:
        """Grava o comando como `enviado` e publica. Levanta ErroContrato ou LookupError antes de gravar."""
        agora = agora or _agora()
        validar_comando(acao, params, expira_s)
        if not self.banco.um("SELECT 1 FROM dispositivos WHERE device_id = %s", (device_id,)):
            raise LookupError(f"dispositivo {device_id} não cadastrado")
        command_id = str(uuid.uuid4())
        self.banco.executar(
            "INSERT INTO comandos (command_id, device_id, origem, acao, params, usuario_id, estado, enviado_em) "
            "VALUES (%s, %s, %s, %s, %s, %s, 'enviado', %s)",
            (command_id, device_id, origem, acao, Jsonb(params), usuario_id, agora))
        try:
            self.publicar(f"{PREFIXO}/{device_id}/comando",
                          montar_comando(device_id, command_id, origem, acao, params, expira_s, agora))
            log.info("comando %s (%s) enviado a %s por %s", command_id, acao, device_id, origem)
        except BrokerIndisponivel as e:
            self.banco.executar("UPDATE comandos SET estado = 'sem_resposta', motivo = %s WHERE command_id = %s",
                                (f"não publicado: {e}", command_id))
        return self.comando(command_id)

    def comando(self, command_id: str) -> dict | None:
        return self.banco.um("SELECT * FROM comandos WHERE command_id = %s", (command_id,))

    def _avancar(self, command_id: str, device_id: str, evento: str, agora: datetime,
                 erro: str | None = None, motivo: str | None = None) -> dict | None:
        """Aplica um evento ao comando de forma atômica; devolve o comando atualizado ou None."""
        with self.banco.pool.connection() as c:
            cmd = c.execute("SELECT * FROM comandos WHERE command_id = %s AND device_id = %s FOR UPDATE",
                            (command_id, device_id)).fetchone()
            novo = cmd and regras.proximo_estado(cmd["estado"], evento)
            if not novo:
                return None
            coluna = _COLUNA_DO_EVENTO[evento]
            return c.execute(
                f"UPDATE comandos SET estado = %s, {coluna} = COALESCE({coluna}, %s), "
                "erro = COALESCE(%s, erro), motivo = COALESCE(%s, motivo) WHERE command_id = %s RETURNING *",
                (novo, agora, erro, motivo, command_id)).fetchone()

    # --- Alertas -----------------------------------------------------------

    def abrir_alerta(self, device_id: str, tipo: str, detalhe: str, agora: datetime,
                     titulo: str, mensagem: str) -> bool:
        """Abre o alerta e notifica no ntfy, uma vez por episódio (índice único de alerta aberto)."""
        aberto = self.banco.um(
            "INSERT INTO alertas (device_id, tipo, detalhe, aberto_em) VALUES (%s, %s, %s, %s) "
            "ON CONFLICT (device_id, tipo) WHERE encerrado_em IS NULL DO NOTHING RETURNING id",
            (device_id, tipo, detalhe, agora))
        if not aberto:
            return False
        log.warning("alerta %s aberto em %s: %s", tipo, device_id, detalhe)
        if self.notificar.enviar(titulo, mensagem, urgente=tipo == "abertura_armado"):
            self.banco.executar("UPDATE alertas SET notificado_em = %s WHERE id = %s", (_agora(), aberto["id"]))
        return True

    def fechar_alerta(self, device_id: str, tipo: str, agora: datetime) -> None:
        self.banco.executar("UPDATE alertas SET encerrado_em = %s "
                            "WHERE device_id = %s AND tipo = %s AND encerrado_em IS NULL", (agora, device_id, tipo))

    # --- Ciclo periódico: prazo dos comandos, regras e agendamentos --------

    def ciclo(self, agora: datetime | None = None) -> None:
        agora = agora or _agora()
        self.banco.executar(
            "UPDATE comandos SET estado = 'sem_resposta', motivo = COALESCE(motivo, %s) "
            "WHERE estado = 'enviado' AND enviado_em < %s",
            (f"sem resposta do dispositivo em {regras.TIMEOUT_RESPOSTA_S} s",
             agora - timedelta(seconds=regras.TIMEOUT_RESPOSTA_S)))
        self._regras(agora)
        self._agendamentos(agora)

    def dispositivos_para_regras(self) -> list[regras.Dispositivo]:
        linhas = self.banco.todos(
            "SELECT d.device_id, d.local, d.online, d.ultimo_contato, d.estado, d.aberta_desde, "
            "EXISTS (SELECT 1 FROM alertas a WHERE a.device_id = d.device_id AND a.tipo = 'abertura_armado' "
            "        AND a.encerrado_em IS NULL) AS alerta_abertura, "
            "EXISTS (SELECT 1 FROM alertas a WHERE a.device_id = d.device_id AND a.tipo = 'dispositivo_offline' "
            "        AND a.encerrado_em IS NULL) AS alerta_offline "
            "FROM dispositivos d ORDER BY d.device_id")
        return [regras.Dispositivo(**linha) for linha in linhas]

    def _regras(self, agora: datetime) -> None:
        limite = self.banco.um("SELECT valor FROM configuracoes WHERE chave = 'limiteEscalonamento_s'")["valor"]
        todos = self.dispositivos_para_regras()
        for d in todos:
            if regras.deve_escalonar(d, limite, agora):
                minutos = int((agora - d.aberta_desde).total_seconds() // 60)
                aberto = self.abrir_alerta(
                    d.device_id, "abertura_armado", f"aberta há mais de {limite} s com o sistema armado", agora,
                    f"Invasão: {d.device_id} aberta", f"Aberta há {minutos} min com o sistema armado. "
                    "Disparando as sirenes do local.")
                if aberto:
                    for alvo in regras.alvos_do_disparo(d, todos):
                        self.enviar_comando(alvo, "disparar", {}, 30, "regra", agora=agora)
            if regras.deve_alertar_offline(d, agora):
                self.abrir_alerta(d.device_id, "dispositivo_offline", "sem contato com o sistema armado", agora,
                                  f"{d.device_id} offline", "Dispositivo armado está sem contato há mais de 1 min.")

    def _agendamentos(self, agora: datetime) -> None:
        local = agora.astimezone(self.fuso)
        for ag in self.banco.todos("SELECT * FROM agendamentos WHERE ativo"):
            if regras.agendamento_vence((ag["hora"].hour, ag["hora"].minute), ag["dias_semana"],
                                        ag["ultimo_disparo"], local):
                self.banco.executar("UPDATE agendamentos SET ultimo_disparo = %s WHERE id = %s", (agora, ag["id"]))
                self.enviar_comando(ag["device_id"], ag["acao"], {}, 30, "agendamento", agora=agora)

    def configurar_limite(self, valor: int) -> None:
        self.banco.executar("UPDATE configuracoes SET valor = %s WHERE chave = 'limiteEscalonamento_s'", (valor,))

