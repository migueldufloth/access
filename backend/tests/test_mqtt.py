"""Testes unitários do cliente MQTT do backend (R4, R7 e R10.2)."""

from types import SimpleNamespace

import paho.mqtt.client as mqtt
import pytest

from app.mqtt import ASSINATURAS, BrokerIndisponivel, ClienteMqtt


class _ClientePahoFalso:
    def __init__(self, conectado: bool = True, rc: int = mqtt.MQTT_ERR_SUCCESS):
        self._conectado = conectado
        self._rc = rc
        self.publicacoes = []
        self.assinaturas = []

    def is_connected(self) -> bool:
        return self._conectado

    def publish(self, topico: str, payload: bytes, qos: int, retain: bool):
        self.publicacoes.append((topico, payload, qos, retain))
        return SimpleNamespace(rc=self._rc)

    def subscribe(self, topicos):
        self.assinaturas.append(topicos)


def test_publicar_desconectado_levanta_broker_indisponivel():
    cli = ClienteMqtt("localhost", 1883, "access_backend", "segredo", lambda *_a: None)
    cli.cliente = _ClientePahoFalso(conectado=False)

    with pytest.raises(BrokerIndisponivel, match="indisponível"):
        cli.publicar("access/grupo5/porta-01/comando", b"{}")


def test_publicar_com_erro_de_retorno_levanta_broker_indisponivel():
    cli = ClienteMqtt("localhost", 1883, "access_backend", "segredo", lambda *_a: None)
    cli.cliente = _ClientePahoFalso(conectado=True, rc=mqtt.MQTT_ERR_NO_CONN)

    with pytest.raises(BrokerIndisponivel):
        cli.publicar("access/grupo5/porta-01/comando", b"{}")


def test_publicar_conectado_usa_qos_1_sem_retain():
    cli = ClienteMqtt("localhost", 1883, "access_backend", "segredo", lambda *_a: None)
    falso = _ClientePahoFalso(conectado=True)
    cli.cliente = falso

    cli.publicar("access/grupo5/porta-01/comando", b'{"acao":"armar"}')

    assert falso.publicacoes == [("access/grupo5/porta-01/comando", b'{"acao":"armar"}', 1, False)]


def test_conectou_refaz_assinaturas_apenas_em_sucesso():
    cli = ClienteMqtt("localhost", 1883, "access_backend", "segredo", lambda *_a: None)
    falso = _ClientePahoFalso()

    cli._conectou(falso, None, None, SimpleNamespace(is_failure=True), None)
    assert falso.assinaturas == []

    cli._conectou(falso, None, None, SimpleNamespace(is_failure=False), None)
    assert falso.assinaturas == [ASSINATURAS]


def test_mensagem_encaminha_e_nao_propaga_excecao():
    recebidas = []

    def ao_receber(topico: str, payload: bytes):
        recebidas.append((topico, payload))
        if payload == b"quebra":
            raise ValueError("erro inesperado")

    cli = ClienteMqtt("localhost", 1883, "access_backend", "segredo", ao_receber)
    cli._mensagem(None, None, SimpleNamespace(topic="access/grupo5/porta-01/estado", payload=b"ok"))
    cli._mensagem(None, None, SimpleNamespace(topic="access/grupo5/porta-01/estado", payload=b"quebra"))

    assert recebidas == [
        ("access/grupo5/porta-01/estado", b"ok"),
        ("access/grupo5/porta-01/estado", b"quebra"),
    ]
