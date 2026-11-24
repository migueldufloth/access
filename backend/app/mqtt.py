"""Cliente MQTT do backend: assina os tópicos dos dispositivos e é o único que publica comandos."""

import logging
from typing import Callable

import paho.mqtt.client as mqtt

from .contrato import PREFIXO

log = logging.getLogger(__name__)

# QoS por tópico, conforme a tabela da seção 2 do contrato.
ASSINATURAS = [(f"{PREFIXO}/+/telemetria", 0), (f"{PREFIXO}/+/estado", 1),
               (f"{PREFIXO}/+/confirmacao", 1), (f"{PREFIXO}/+/status", 1)]


class BrokerIndisponivel(Exception):
    pass


class ClienteMqtt:
    def __init__(self, host: str, porta: int, usuario: str, senha: str,
                 ao_receber: Callable[[str, bytes], None]):
        self.host, self.porta, self.ao_receber = host, porta, ao_receber
        self.cliente = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="access-backend", clean_session=True)
        self.cliente.username_pw_set(usuario, senha)
        self.cliente.reconnect_delay_set(1, 30)
        self.cliente.on_connect = self._conectou
        self.cliente.on_disconnect = self._desconectou
        self.cliente.on_message = self._mensagem

    def iniciar(self) -> None:
        # connect_async + loop_start: se o broker estiver fora, o paho tenta de novo sozinho.
        self.cliente.connect_async(self.host, self.porta, keepalive=30)
        self.cliente.loop_start()

    def parar(self) -> None:
        self.cliente.disconnect()
        self.cliente.loop_stop()

    def publicar(self, topico: str, payload: bytes) -> None:
        """Comando: QoS 1, sem retain. Com o broker fora, falha na hora em vez de enfileirar
        (um comando atrasado não pode ser executado quando a conexão voltar)."""
        if not self.cliente.is_connected():
            raise BrokerIndisponivel("broker MQTT indisponível")
        info = self.cliente.publish(topico, payload, qos=1, retain=False)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            raise BrokerIndisponivel(mqtt.error_string(info.rc))

    def _conectou(self, cliente, _userdata, _flags, reason_code, _props) -> None:
        if reason_code.is_failure:
            log.error("broker recusou a conexão: %s", reason_code)
            return
        log.info("conectado ao broker %s:%s; refazendo assinaturas", self.host, self.porta)
        cliente.subscribe(ASSINATURAS)

    def _desconectou(self, _cliente, _userdata, _flags, reason_code, _props) -> None:
        log.warning("desconectado do broker (%s); reconectando", reason_code)

    def _mensagem(self, _cliente, _userdata, msg) -> None:
        try:
            self.ao_receber(msg.topic, msg.payload)
        except Exception:  # uma mensagem com problema não pode derrubar o laço do MQTT
            log.exception("erro ao tratar mensagem de %s", msg.topic)
