"""Validação do contrato v1: um caso por regra da seção 6 do CONTRATOS.md, mais os comandos da seção 4."""

import json

import pytest

from app.contrato import ErroContrato, separar_topico, validar_comando, validar_mensagem

TS = "2026-11-10T22:15:03Z"


def telemetria(**extra):
    msg = {"v": 1, "deviceId": "porta-01", "ts": TS, "uptime_s": 10,
           "medicoes": [{"grandeza": "abertura", "valor": 1, "unidade": "bool"},
                        {"grandeza": "rssi", "valor": -52, "unidade": "dBm"}]}
    msg.update(extra)
    return msg


def estado(**extra):
    msg = {"v": 1, "deviceId": "porta-01", "ts": TS, "uptime_s": 10, "fw": "1.0.0", "modo": "automatico",
           "armado": True, "alarme": "inativo", "sensor": "ok",
           "config": {"tempoEntrada_s": 20, "intervaloTelemetria_s": 5, "timeoutManual_s": 1800},
           "ultimoComandoId": None}
    msg.update(extra)
    return msg


def validar(tipo, msg, device="porta-01"):
    bruto = msg if isinstance(msg, bytes) else json.dumps(msg).encode()
    return validar_mensagem(tipo, device, bruto)


def codigo(tipo, msg, device="porta-01"):
    with pytest.raises(ErroContrato) as e:
        validar(tipo, msg, device)
    return e.value.codigo


# --- Mensagens válidas -------------------------------------------------------

def test_telemetria_valida():
    assert validar("telemetria", telemetria())["_ts"].year == 2026


def test_ts_null_e_aceito():
    assert validar("telemetria", telemetria(ts=None))["_ts"] is None


def test_estado_valido():
    validar("estado", estado(ultimoComandoId="7c9e6679-7425-40de-944b-e07fc1f90ae7"))


def test_confirmacao_valida():
    validar("confirmacao", {"v": 1, "deviceId": "porta-01", "ts": TS, "uptime_s": 1,
                            "commandId": "7c9e6679-7425-40de-944b-e07fc1f90ae7", "fase": "recebido",
                            "resultado": "rejeitado", "erro": "E_DESARMADO", "motivo": "x", "duplicado": False})


def test_last_will_sem_ts_nem_uptime():
    assert validar("status", {"v": 1, "deviceId": "porta-01", "status": "offline"})["_ts"] is None


# --- Regras de descarte da seção 6 -------------------------------------------

def test_regra1_nao_json_ou_grande_demais():
    assert codigo("telemetria", b"porta aberta") == "E_JSON_INVALIDO"
    assert codigo("telemetria", b"x" * 513) == "E_JSON_INVALIDO"
    assert codigo("telemetria", b"[1, 2]") == "E_JSON_INVALIDO"


def test_regra2_versao_diferente_de_1():
    assert codigo("telemetria", telemetria(v=2)) == "E_VERSAO_NAO_SUPORTADA"


def test_regra3_device_id_diferente_do_topico():
    assert codigo("telemetria", telemetria(), device="janela-01") == "E_DEVICE_ID_INCORRETO"


def test_regra4_falta_ts_ou_uptime():
    sem_ts = telemetria()
    del sem_ts["ts"]
    assert codigo("telemetria", sem_ts) == "E_CAMPO_AUSENTE"
    sem_uptime = telemetria()
    del sem_uptime["uptime_s"]
    assert codigo("telemetria", sem_uptime) == "E_CAMPO_AUSENTE"
    assert codigo("status", {"v": 1, "deviceId": "porta-01", "status": "online"}) == "E_CAMPO_AUSENTE"


def test_regra5_grandeza_ou_unidade_fora_da_tabela():
    assert codigo("telemetria", telemetria(medicoes=[{"grandeza": "temperatura", "valor": 1, "unidade": "C"}])) \
        == "E_CAMPO_AUSENTE"
    assert codigo("telemetria", telemetria(medicoes=[{"grandeza": "rssi", "valor": -50, "unidade": "dB"}])) \
        == "E_CAMPO_AUSENTE"
    assert codigo("telemetria", telemetria(medicoes=[{"grandeza": "abertura", "valor": True, "unidade": "bool"}])) \
        == "E_CAMPO_AUSENTE"


def test_regra6_valor_fora_da_faixa():
    assert codigo("telemetria", telemetria(medicoes=[{"grandeza": "rssi", "valor": 50, "unidade": "dBm"}])) \
        == "E_VALOR_FORA_FAIXA"
    assert codigo("estado", estado(config={"tempoEntrada_s": 500})) == "E_VALOR_FORA_FAIXA"


def test_estado_com_enum_invalido():
    assert codigo("estado", estado(alarme="tocando")) == "E_CAMPO_AUSENTE"
    assert codigo("estado", estado(armado="sim")) == "E_CAMPO_AUSENTE"


def test_confirmacao_com_resultado_incoerente_com_a_fase():
    assert codigo("confirmacao", {"v": 1, "deviceId": "porta-01", "ts": TS, "uptime_s": 1,
                                  "commandId": "7c9e6679-7425-40de-944b-e07fc1f90ae7", "fase": "executado",
                                  "resultado": "aceito", "erro": None, "motivo": None}) == "E_CAMPO_AUSENTE"


def test_topico():
    assert separar_topico("access/grupo5/porta-01/telemetria") == ("porta-01", "telemetria")
    assert separar_topico("access/grupo5/porta-01/comando") is None
    assert separar_topico("access/grupo5/sensor/porta") is None


# --- Comandos ------------------------------------------------------------------

@pytest.mark.parametrize("acao, params, expira, erro", [
    ("abrir", {}, 30, "E_ACAO_DESCONHECIDA"),
    ("armar", {"x": 1}, 30, "E_VALOR_FORA_FAIXA"),
    ("armar", {}, 500, "E_VALOR_FORA_FAIXA"),
    ("configurar", {}, 30, "E_CAMPO_AUSENTE"),
    ("configurar", {"tempoEntrada_s": 121}, 30, "E_VALOR_FORA_FAIXA"),
    ("configurar", {"volume": 3}, 30, "E_VALOR_FORA_FAIXA"),
    ("atualizar_firmware", {"url": "http://x/a.bin", "sha256": "a" * 64, "versao": "1.0.1"}, 60, "E_VALOR_FORA_FAIXA"),
])
def test_comando_invalido(acao, params, expira, erro):
    with pytest.raises(ErroContrato) as e:
        validar_comando(acao, params, expira)
    assert e.value.codigo == erro


def test_comandos_validos():
    validar_comando("armar", {}, 30)
    validar_comando("configurar", {"tempoEntrada_s": 0, "timeoutManual_s": 7200}, 5)
    validar_comando("atualizar_firmware", {"url": "https://h/f.bin", "sha256": "0" * 64, "versao": "1.0.1"}, 120)
