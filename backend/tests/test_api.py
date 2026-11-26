"""Integração com PostgreSQL real: ingestão, ciclo de comando, regras, agendamento, segurança e analítico."""

import json
from datetime import datetime, timedelta, timezone

from conftest import token

T0 = datetime(2026, 11, 24, 12, 0, tzinfo=timezone.utc)


def publicar(servico, device, tipo, agora=T0, **campos):
    msg = {"v": 1, "deviceId": device, "ts": None, "uptime_s": 5, **campos}
    servico.ingerir(f"access/grupo5/{device}/{tipo}", json.dumps(msg).encode(), agora)


def estado(servico, device="porta-01", agora=T0, **campos):
    base = {"fw": "1.0.0", "modo": "automatico", "armado": True, "alarme": "inativo", "sensor": "ok",
            "config": {"intervaloTelemetria_s": 5}, "ultimoComandoId": None}
    publicar(servico, device, "estado", agora, **{**base, **campos})


def porta(servico, aberta, device="porta-01", agora=T0):
    publicar(servico, device, "telemetria", agora,
             medicoes=[{"grandeza": "abertura", "valor": int(aberta), "unidade": "bool"},
                       {"grandeza": "rssi", "valor": -60, "unidade": "dBm"}])


def confirmar(servico, cmd_id, fase, resultado, device="porta-01", erro=None, motivo=None, duplicado=False):
    publicar(servico, device, "confirmacao", commandId=cmd_id, fase=fase, resultado=resultado,
             erro=erro, motivo=motivo, duplicado=duplicado)


# --- R9 / R16: autenticação e perfis --------------------------------------------

def test_rotas_exigem_token_e_comando_exige_admin(ambiente):
    cliente, *_ = ambiente
    assert cliente.get("/api/dispositivos").status_code == 401
    assert cliente.post("/api/comandos", json={"deviceId": "porta-01", "acao": "armar"}).status_code == 401
    assert cliente.post("/api/auth/login", json={"usuario": "admin", "senha": "errada"}).status_code == 401
    oper = token(cliente, "operador", "oper123")
    assert cliente.get("/api/dispositivos", headers=oper).status_code == 200
    r = cliente.post("/api/comandos", json={"deviceId": "porta-01", "acao": "armar"}, headers=oper)
    assert r.status_code == 403


# --- R4 / R5: ingestão validada e persistida ---------------------------------------

def test_telemetria_grava_e_aparece_na_api(ambiente):
    cliente, servico, *_ = ambiente
    porta(servico, True)
    h = token(cliente)
    disp = {d["device_id"]: d for d in cliente.get("/api/dispositivos", headers=h).json()}
    assert disp["porta-01"]["online"] is True
    assert {m["grandeza"]: m["valor"] for m in disp["porta-01"]["ultimas"]} == {"abertura": 1, "rssi": -60}
    hist = cliente.get("/api/dispositivos/porta-01/medicoes?grandeza=abertura", headers=h).json()
    assert [m["valor"] for m in hist] == [1]


def test_mensagem_invalida_e_descartada_e_registrada(ambiente):
    cliente, servico, *_ = ambiente
    servico.ingerir("access/grupo5/porta-01/telemetria", b'{"v":1,"deviceId":"porta-01"}', T0)
    servico.ingerir("access/grupo5/porta-01/telemetria", b"nao sou json", T0)
    invalidas = cliente.get("/api/mensagens-invalidas", headers=token(cliente)).json()
    assert {i["motivo"].split(":")[0] for i in invalidas} == {"E_CAMPO_AUSENTE", "E_JSON_INVALIDO"}
    assert servico.banco.um("SELECT count(*) AS n FROM medicoes")["n"] == 0


def test_last_will_marca_offline_e_registra_evento(ambiente):
    _, servico, *_ = ambiente
    porta(servico, False)
    publicar(servico, "porta-01", "status", T0 + timedelta(seconds=30), status="offline")
    assert servico.banco.um("SELECT online FROM dispositivos WHERE device_id = 'porta-01'")["online"] is False
    eventos = servico.banco.todos("SELECT status FROM eventos_status ORDER BY id")
    assert [e["status"] for e in eventos] == ["online", "offline"]


# --- R7: ciclo do comando --------------------------------------------------------

def test_comando_enviado_recebido_executado_confirmado(ambiente):
    cliente, servico, broker, _ = ambiente
    h = token(cliente)
    r = cliente.post("/api/comandos", json={"deviceId": "porta-01", "acao": "armar"}, headers=h)
    assert r.status_code == 202 and r.json()["estado"] == "enviado"
    cmd_id = r.json()["command_id"]
    topico, payload = broker.publicados[-1]
    assert topico == "access/grupo5/porta-01/comando"
    assert payload["commandId"] == cmd_id and payload["origem"] == "usuario" and payload["uptime_s"] is None

    confirmar(servico, cmd_id, "recebido", "aceito")
    assert cliente.get(f"/api/comandos/{cmd_id}", headers=h).json()["estado"] == "recebido"
    confirmar(servico, cmd_id, "executado", "ok")
    assert cliente.get(f"/api/comandos/{cmd_id}", headers=h).json()["estado"] == "executado"
    estado(servico, ultimoComandoId=cmd_id)
    final = cliente.get(f"/api/comandos/{cmd_id}", headers=h).json()
    assert final["estado"] == "confirmado"
    assert final["recebido_em"] and final["executado_em"] and final["confirmado_em"]

    confirmar(servico, cmd_id, "recebido", "aceito", duplicado=True)   # duplicado não regride
    assert cliente.get(f"/api/comandos/{cmd_id}", headers=h).json()["estado"] == "confirmado"


def test_comando_rejeitado_guarda_motivo(ambiente):
    cliente, servico, *_ = ambiente
    h = token(cliente)
    cmd_id = cliente.post("/api/comandos", json={"deviceId": "porta-01", "acao": "disparar"}, headers=h).json()["command_id"]
    confirmar(servico, cmd_id, "recebido", "rejeitado", erro="E_DESARMADO", motivo="disparar exige o sistema armado")
    cmd = cliente.get(f"/api/comandos/{cmd_id}", headers=h).json()
    assert (cmd["estado"], cmd["erro"], cmd["motivo"]) == ("rejeitado", "E_DESARMADO", "disparar exige o sistema armado")


def test_comando_fora_do_contrato_nao_e_publicado(ambiente):
    cliente, _, broker, _ = ambiente
    h = token(cliente)
    r = cliente.post("/api/comandos", json={"deviceId": "porta-01", "acao": "configurar",
                                            "params": {"tempoEntrada_s": 999}}, headers=h)
    assert r.status_code == 422 and r.json()["erro"] == "E_VALOR_FORA_FAIXA"
    r = cliente.post("/api/comandos", json={"deviceId": "nao-existe", "acao": "armar"}, headers=h)
    assert r.status_code == 404
    assert broker.publicados == []


def test_sem_resposta_em_10s_e_broker_fora(ambiente):
    cliente, servico, broker, _ = ambiente
    h = token(cliente)
    cmd = servico.enviar_comando("porta-01", "armar", {}, 30, "usuario", agora=T0)
    servico.ciclo(T0 + timedelta(seconds=11))
    assert servico.comando(cmd["command_id"])["estado"] == "sem_resposta"

    broker.online = False
    r = cliente.post("/api/comandos", json={"deviceId": "porta-01", "acao": "armar"}, headers=h)
    assert r.json()["estado"] == "sem_resposta" and "não publicado" in r.json()["motivo"]


# --- R8: regra remota, R15: notificação ----------------------------------------------

def test_escalonamento_dispara_sirenes_do_local_e_notifica(ambiente):
    _, servico, broker, ntfy = ambiente
    estado(servico, "porta-01", alarme="disparado")
    estado(servico, "janela-01", armado=True)
    porta(servico, True, agora=T0)

    servico.ciclo(T0 + timedelta(seconds=30))
    assert broker.publicados == []                       # ainda dentro do limite de 60 s

    servico.ciclo(T0 + timedelta(seconds=61))
    alvos = sorted(p["deviceId"] for _, p in broker.publicados)
    assert alvos == ["janela-01", "porta-01"]
    assert all(p["acao"] == "disparar" and p["origem"] == "regra" for _, p in broker.publicados)
    urgentes = [n for n in ntfy.enviadas if n[2]]   # os dois também ficaram 61 s calados: alerta offline à parte
    assert len(urgentes) == 1 and "porta-01" in urgentes[0][0]
    alerta = servico.banco.um("SELECT * FROM alertas WHERE tipo = 'abertura_armado'")
    assert alerta["notificado_em"] is not None

    servico.ciclo(T0 + timedelta(seconds=90))           # um alerta por episódio
    assert len(broker.publicados) == 2
    porta(servico, False, agora=T0 + timedelta(seconds=95))
    assert servico.banco.um("SELECT encerrado_em FROM alertas WHERE tipo = 'abertura_armado'")["encerrado_em"]


def test_desarmar_encerra_o_alerta_de_invasao(ambiente):
    _, servico, *_ = ambiente
    estado(servico, alarme="disparado")
    porta(servico, True)
    servico.ciclo(T0 + timedelta(seconds=61))
    assert servico.banco.um("SELECT count(*) AS n FROM alertas WHERE encerrado_em IS NULL "
                            "AND tipo = 'abertura_armado'")["n"] == 1
    estado(servico, armado=False, alarme="inativo")   # porta continua aberta
    assert servico.banco.um("SELECT count(*) AS n FROM alertas WHERE encerrado_em IS NULL "
                            "AND tipo = 'abertura_armado'")["n"] == 0


def test_limite_configuravel_pela_interface(ambiente):
    cliente, servico, broker, _ = ambiente
    h = token(cliente)
    assert cliente.put("/api/configuracao", json={"limiteEscalonamento_s": 5},
                       headers=token(cliente, "operador", "oper123")).status_code == 403
    assert cliente.put("/api/configuracao", json={"limiteEscalonamento_s": 5}, headers=h).status_code == 422
    assert cliente.put("/api/configuracao", json={"limiteEscalonamento_s": 10}, headers=h).status_code == 200
    estado(servico, alarme="entrada")
    porta(servico, True)
    servico.ciclo(T0 + timedelta(seconds=11))
    assert len(broker.publicados) == 1


def test_falha_de_sensor_abre_e_fecha_alerta(ambiente):
    _, servico, _, ntfy = ambiente
    estado(servico, sensor="falha_aberto", modo="seguro")
    estado(servico, sensor="falha_aberto", modo="seguro")
    assert len(ntfy.enviadas) == 1
    estado(servico, sensor="ok")
    assert servico.banco.um("SELECT encerrado_em FROM alertas WHERE tipo = 'falha_sensor'")["encerrado_em"]


def test_offline_armado_gera_alerta(ambiente):
    _, servico, _, ntfy = ambiente
    estado(servico, armado=True)
    publicar(servico, "porta-01", "status", T0, status="offline")
    servico.ciclo(T0 + timedelta(seconds=61))
    assert servico.banco.um("SELECT count(*) AS n FROM alertas WHERE tipo = 'dispositivo_offline' "
                            "AND encerrado_em IS NULL")["n"] == 1
    porta(servico, False, agora=T0 + timedelta(seconds=70))   # voltou: alerta encerrado
    assert servico.banco.um("SELECT count(*) AS n FROM alertas WHERE encerrado_em IS NULL")["n"] == 0


def test_alerta_offline_por_silencio_fecha_quando_a_telemetria_volta(ambiente):
    _, servico, *_ = ambiente
    estado(servico, armado=True)                          # online, depois fica calado sem Last Will
    servico.ciclo(T0 + timedelta(seconds=61))
    assert servico.banco.um("SELECT count(*) AS n FROM alertas WHERE encerrado_em IS NULL")["n"] == 1
    porta(servico, False, agora=T0 + timedelta(seconds=65))
    assert servico.banco.um("SELECT count(*) AS n FROM alertas WHERE encerrado_em IS NULL")["n"] == 0


def test_agendamento_gera_comando_e_rejeicao_vira_alerta(ambiente):
    cliente, servico, broker, ntfy = ambiente
    h = token(cliente)
    r = cliente.post("/api/agendamentos", json={"device_id": "porta-01", "acao": "armar", "hora": "22:00",
                                                "dias_semana": [1]}, headers=h)
    assert r.status_code == 201
    terca_22h_sp = datetime(2026, 11, 25, 1, 0, 20, tzinfo=timezone.utc)   # 22:00 de terça em São Paulo
    servico.ciclo(terca_22h_sp)
    servico.ciclo(terca_22h_sp + timedelta(seconds=5))
    assert len(broker.publicados) == 1
    cmd_id = broker.publicados[0][1]["commandId"]
    assert broker.publicados[0][1]["origem"] == "agendamento"
    confirmar(servico, cmd_id, "recebido", "rejeitado", erro="E_PORTA_ABERTA", motivo="porta aberta")
    assert servico.banco.um("SELECT tipo FROM alertas")["tipo"] == "agendamento_nao_aplicado"
    assert len(ntfy.enviadas) == 1


# --- R14: analítico ----------------------------------------------------------------

def test_analitico_usa_historico_real(ambiente):
    cliente, servico, *_ = ambiente
    agora = datetime.now(timezone.utc)
    porta(servico, True, agora=agora - timedelta(minutes=10))
    porta(servico, False, agora=agora - timedelta(minutes=9))
    porta(servico, True, agora=agora - timedelta(minutes=5))
    estado(servico, alarme="disparado", agora=agora - timedelta(minutes=5))
    estado(servico, alarme="inativo", agora=agora - timedelta(minutes=4))
    a = cliente.get("/api/analitico/porta-01", headers=token(cliente)).json()
    assert sum(h["eventos"] for h in a["eventos_por_hora"]) == 2
    assert 55 <= a["tempo_em_alarme_s"] <= 65
    assert a["uptime_pct"] == 100   # online desde o primeiro contato, há 10 min
    assert len(a["rssi"]) == 3 and a["rssi"][-1]["media"] == -60


def test_firmware_publico_so_para_bin(ambiente):
    cliente, *_ = ambiente
    (cliente.app.state.config.firmware_dir / "access-1.0.1.bin").write_bytes(b"\x00binario")
    assert cliente.get("/firmware/access-1.0.1.bin").content == b"\x00binario"
    assert cliente.get("/firmware/..%2Fsegredo.bin").status_code == 404
    assert cliente.get("/firmware/nao-existe.bin").status_code == 404


def test_saude_e_identidade_do_usuario(ambiente):
    cliente, *_ = ambiente
    assert cliente.get("/api/saude").json() == {"ok": True}
    eu_admin = cliente.get("/api/eu", headers=token(cliente)).json()
    assert (eu_admin["nome"], eu_admin["perfil"]) == ("admin", "administrador")
    eu_oper = cliente.get("/api/eu", headers=token(cliente, "operador", "oper123")).json()
    assert (eu_oper["nome"], eu_oper["perfil"]) == ("operador", "operador")


def test_listagem_e_consulta_de_comandos(ambiente):
    cliente, *_ = ambiente
    h = token(cliente)
    cmd_id = cliente.post("/api/comandos", json={"deviceId": "porta-01", "acao": "armar"}, headers=h).json()["command_id"]
    lista = cliente.get("/api/comandos", headers=h).json()
    assert len(lista) == 1 and lista[0]["command_id"] == cmd_id and lista[0]["nome_usuario"] == "admin"
    assert cliente.get("/api/comandos/nao-e-uuid", headers=h).status_code == 404
    assert cliente.get("/api/comandos/00000000-0000-0000-0000-000000000000", headers=h).status_code == 404


def test_comando_falha_na_execucao_e_resposta_atrasada(ambiente):
    cliente, servico, *_ = ambiente
    h = token(cliente)
    cmd_id = cliente.post("/api/comandos", json={"deviceId": "porta-01", "acao": "armar"}, headers=h).json()["command_id"]
    confirmar(servico, cmd_id, "recebido", "aceito")
    confirmar(servico, cmd_id, "executado", "falha", erro="E_INTERNO", motivo="falha na sirene")
    cmd = cliente.get(f"/api/comandos/{cmd_id}", headers=h).json()
    assert (cmd["estado"], cmd["erro"], cmd["motivo"]) == ("falha", "E_INTERNO", "falha na sirene")

    atrasado = servico.enviar_comando("porta-01", "silenciar", {}, 30, "usuario", agora=T0)
    servico.ciclo(T0 + timedelta(seconds=11))
    assert servico.comando(atrasado["command_id"])["estado"] == "sem_resposta"
    confirmar(servico, atrasado["command_id"], "recebido", "aceito")
    assert servico.comando(atrasado["command_id"])["estado"] == "recebido"


def test_crud_e_validacao_de_agendamentos(ambiente):
    cliente, *_ = ambiente
    h_admin = token(cliente)
    h_oper = token(cliente, "operador", "oper123")

    assert cliente.post("/api/agendamentos", json={"device_id": "porta-01", "acao": "armar", "hora": "22:00",
                                                   "dias_semana": [1]}, headers=h_oper).status_code == 403
    assert cliente.post("/api/agendamentos", json={"device_id": "porta-01", "acao": "armar", "hora": "22:00",
                                                   "dias_semana": [7]}, headers=h_admin).status_code == 422
    assert cliente.post("/api/agendamentos", json={"device_id": "nao-existe", "acao": "armar", "hora": "22:00",
                                                   "dias_semana": [1]}, headers=h_admin).status_code == 404

    criado = cliente.post("/api/agendamentos", json={"device_id": "porta-01", "acao": "armar", "hora": "22:00",
                                                     "dias_semana": [5, 1, 5]}, headers=h_admin)
    assert criado.status_code == 201 and criado.json()["dias_semana"] == [1, 5]
    ag_id = criado.json()["id"]

    assert len(cliente.get("/api/agendamentos", headers=h_oper).json()) == 1
    assert cliente.delete(f"/api/agendamentos/{ag_id}", headers=h_oper).status_code == 403
    assert cliente.delete(f"/api/agendamentos/{ag_id}", headers=h_admin).status_code == 204
    assert cliente.delete(f"/api/agendamentos/{ag_id}", headers=h_admin).status_code == 404


def test_alertas_ativos_e_filtros_de_medicoes(ambiente):
    cliente, servico, *_ = ambiente
    h = token(cliente)

    estado(servico, sensor="falha_aberto", modo="seguro", agora=T0)
    assert len(cliente.get("/api/alertas?ativos=true", headers=h).json()) == 1
    estado(servico, sensor="ok", agora=T0 + timedelta(seconds=10))
    assert cliente.get("/api/alertas?ativos=true", headers=h).json() == []
    todos = cliente.get("/api/alertas?ativos=false", headers=h).json()
    assert len(todos) == 1 and todos[0]["encerrado_em"] is not None

    porta(servico, True, agora=T0)
    porta(servico, False, agora=T0 + timedelta(minutes=5))
    porta(servico, True, agora=T0 + timedelta(minutes=10))
    de = (T0 + timedelta(minutes=1)).isoformat()
    ate = (T0 + timedelta(minutes=9)).isoformat()
    janela = cliente.get("/api/dispositivos/porta-01/medicoes",
                         params={"grandeza": "abertura", "de": de, "ate": ate}, headers=h).json()
    assert [m["valor"] for m in janela] == [0]
    ultimas = cliente.get("/api/dispositivos/porta-01/medicoes",
                          params={"grandeza": "abertura", "limite": 2}, headers=h).json()
    assert [m["valor"] for m in ultimas] == [0, 1]
