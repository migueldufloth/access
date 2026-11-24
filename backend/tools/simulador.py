"""Simulador de dispositivo do contrato v1: valida o sistema ponta a ponta sem as placas.

Uso (a partir de backend/):
    python -m tools.simulador porta-01 --senha <MQTT_SENHA_PORTA_01> [--host localhost] [--porta 1883]

Comandos no terminal: abrir, fechar, falha, ok, sair.
Segue o mesmo comportamento esperado do firmware: regra local (entrada -> disparado), confirmação
por fase, rejeições do catálogo de erros, duplicado com `duplicado: true`, Last Will e estado retido.
"""

import argparse
import json
import random
import sys
import threading
import time
from collections import OrderedDict
from datetime import datetime, timedelta, timezone

import paho.mqtt.client as mqtt

from app.contrato import PREFIXO, ErroContrato, iso, parse_ts, validar_comando

FW = "1.0.0"


class Dispositivo:
    def __init__(self, device_id: str):
        self.id, self.inicio = device_id, time.monotonic()
        self.presenca = device_id.startswith("janela")
        self.aberta, self.aberturas, self.sensor = False, 0, "ok"
        self.armado, self.alarme, self.modo = False, "inativo", "automatico"
        self.config = {"tempoEntrada_s": 20, "intervaloTelemetria_s": 5, "timeoutManual_s": 1800}
        self.ultimo_comando, self.entrada_ate = None, None
        self.vistos: OrderedDict[str, list[dict]] = OrderedDict()   # últimos 16 commandId
        self.trava = threading.Lock()
        self.cliente: mqtt.Client | None = None

    # --- mensagens -------------------------------------------------------------
    def base(self) -> dict:
        return {"v": 1, "deviceId": self.id, "ts": iso(datetime.now(timezone.utc)),
                "uptime_s": int(time.monotonic() - self.inicio)}

    def pub(self, tipo: str, corpo: dict, qos: int = 1, retain: bool = False) -> None:
        payload = json.dumps({**self.base(), **corpo})
        self.cliente.publish(f"{PREFIXO}/{self.id}/{tipo}", payload, qos=qos, retain=retain)
        print(f"-> {tipo}: {payload}")

    def estado(self) -> None:
        self.pub("estado", {"fw": FW, "modo": self.modo, "armado": self.armado, "alarme": self.alarme,
                            "sensor": self.sensor, "config": self.config,
                            "ultimoComandoId": self.ultimo_comando}, retain=True)

    def telemetria(self) -> None:
        self.pub("telemetria", {"medicoes": [
            {"grandeza": "presenca" if self.presenca else "abertura", "valor": int(self.aberta), "unidade": "bool"},
            {"grandeza": "aberturas", "valor": self.aberturas, "unidade": "contagem"},
            {"grandeza": "rssi", "valor": random.randint(-70, -45), "unidade": "dBm"}]}, qos=0)

    # --- regra local -------------------------------------------------------------
    def mudar_sensor(self, aberta: bool) -> None:
        with self.trava:
            if aberta and not self.aberta:
                self.aberturas += 1
                if self.armado and self.alarme == "inativo":
                    self.alarme = "entrada"
                    self.entrada_ate = time.monotonic() + self.config["tempoEntrada_s"]
            self.aberta = aberta
            self.telemetria()
            self.estado()

    def tique(self) -> None:
        with self.trava:
            if self.alarme == "entrada" and time.monotonic() >= self.entrada_ate:
                self.alarme = "disparado"
                self.estado()

    # --- comandos ----------------------------------------------------------------
    def comando(self, bruto: bytes) -> None:
        with self.trava:
            try:
                cmd = json.loads(bruto)
                cmd_id = cmd.get("commandId")
            except (ValueError, AttributeError):
                cmd, cmd_id = None, None
            if not isinstance(cmd_id, str):
                print("<- comando sem JSON válido ou sem commandId: ignorado (não há a quem responder)")
                return
            if cmd_id in self.vistos:   # reentrega: repete as confirmações sem reexecutar
                for conf in self.vistos[cmd_id]:
                    self.pub("confirmacao", {**conf, "duplicado": True})
                return
            confs = self.executar(cmd, cmd_id)
            self.vistos[cmd_id] = confs
            while len(self.vistos) > 16:
                self.vistos.popitem(last=False)

    def executar(self, cmd: dict, cmd_id: str) -> list[dict]:
        def conf(fase, resultado, erro=None, motivo=None):
            c = {"commandId": cmd_id, "fase": fase, "resultado": resultado, "erro": erro, "motivo": motivo,
                 "duplicado": False}
            self.pub("confirmacao", c)
            return c

        try:
            if cmd.get("v") != 1:
                raise ErroContrato("E_VERSAO_NAO_SUPORTADA", "versão não suportada")
            if cmd.get("deviceId") != self.id:
                raise ErroContrato("E_DEVICE_ID_INCORRETO", "deviceId de outro dispositivo")
            validar_comando(cmd.get("acao"), cmd.get("params"), cmd.get("expira_s"))
            emitido = parse_ts(cmd.get("ts"))
            if emitido and datetime.now(timezone.utc) > emitido + timedelta(seconds=cmd["expira_s"]):
                raise ErroContrato("E_EXPIRADO", "comando expirado")
            acao = cmd["acao"]
            if cmd.get("origem") == "agendamento" and self.modo == "manual":
                raise ErroContrato("E_MODO_MANUAL", "agendamento ignorado no modo manual")
            if acao == "armar" and self.aberta:
                raise ErroContrato("E_PORTA_ABERTA", "feche a porta antes de armar")
            if acao == "armar" and self.sensor != "ok":
                raise ErroContrato("E_SENSOR_FALHA", "sensor em falha")
            if acao == "disparar" and not self.armado:
                raise ErroContrato("E_DESARMADO", "disparar exige o sistema armado")
        except ErroContrato as e:
            return [conf("recebido", "rejeitado", e.codigo, e.motivo)]

        confs = [conf("recebido", "aceito")]
        if acao == "atualizar_firmware":
            return confs + [conf("executado", "falha", "E_OTA_FALHA", "simulador não grava firmware")]
        if acao == "armar":
            self.armado = True
        elif acao == "desarmar":
            self.armado, self.alarme = False, "inativo"
        elif acao == "disparar":
            self.alarme = "disparado"
        elif acao == "silenciar":
            self.alarme = "inativo"
        elif acao == "configurar":
            self.config.update(cmd["params"])
        confs.append(conf("executado", "ok"))
        self.ultimo_comando = cmd_id
        self.estado()
        return confs


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("device_id")
    ap.add_argument("--senha", required=True)
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--porta", type=int, default=1883)
    args = ap.parse_args()

    d = Dispositivo(args.device_id)
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"sim-{args.device_id}", clean_session=True)
    d.cliente = c
    c.username_pw_set(args.device_id, args.senha)
    c.will_set(f"{PREFIXO}/{d.id}/status", json.dumps({"v": 1, "deviceId": d.id, "status": "offline"}),
               qos=1, retain=True)

    def conectou(cl, _u, _f, rc, _p):
        if rc.is_failure:
            print(f"conexão recusada: {rc}")
            return
        cl.subscribe(f"{PREFIXO}/{d.id}/comando", qos=1)
        d.pub("status", {"status": "online", "fw": FW}, retain=True)
        d.estado()

    c.on_connect = conectou
    c.on_message = lambda _c, _u, m: d.comando(m.payload)
    c.reconnect_delay_set(1, 10)
    c.connect_async(args.host, args.porta, keepalive=15)
    c.loop_start()

    def periodico():
        proxima = 0.0
        while True:
            time.sleep(0.5)
            d.tique()
            if c.is_connected() and time.monotonic() >= proxima:
                with d.trava:
                    d.telemetria()
                proxima = time.monotonic() + d.config["intervaloTelemetria_s"]

    threading.Thread(target=periodico, daemon=True).start()
    for linha in sys.stdin:
        acao = linha.strip()
        if acao in ("abrir", "fechar"):
            d.mudar_sensor(acao == "abrir")
        elif acao in ("falha", "ok"):
            with d.trava:
                d.sensor, d.modo = ("falha_aberto", "seguro") if acao == "falha" else ("ok", "automatico")
                d.estado()
        elif acao == "sair":
            break
    d.pub("status", {"status": "offline"}, retain=True)
    c.disconnect()


if __name__ == "__main__":
    main()
