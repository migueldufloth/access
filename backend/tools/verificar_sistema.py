"""Verificação ponta a ponta do sistema rodando no compose, com dois dispositivos simulados.

Cobre R4, R7, R8, R10 (cenários 2, 3, 4 e o Last Will do 1), R13, R14 e a ACL do R16.
Uso (a partir de backend/, com o compose no ar e o .env na raiz do repositório):
    python -m tools.verificar_sistema
Sai com código 1 se algum item falhar. Roda também no CI (.github/workflows/ci.yml).
"""

import json
import subprocess
import sys
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

import httpx
import paho.mqtt.client as mqtt

RAIZ = Path(__file__).resolve().parents[2]
API = "http://localhost:8000/api"
MQTT_HOST = "localhost"


def ler_env() -> dict:
    env = {}
    for linha in (RAIZ / ".env").read_text().splitlines():
        if "=" in linha and not linha.lstrip().startswith("#"):
            chave, valor = linha.split("=", 1)
            env[chave.strip()] = valor.strip()
    return env


ENV = ler_env()
SENHAS = {"porta-01": ENV["MQTT_SENHA_PORTA_01"], "janela-01": ENV["MQTT_SENHA_JANELA_01"]}
falhas = 0


def checar(cond: bool, descricao: str) -> None:
    global falhas
    print(("PASSOU " if cond else "FALHOU ") + descricao, flush=True)
    falhas += not cond


def simulador(device: str) -> subprocess.Popen:
    return subprocess.Popen([sys.executable, "-m", "tools.simulador", device, "--senha", SENHAS[device]],
                            cwd=RAIZ / "backend", stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, text=True)


def cliente_mqtt(usuario: str, senha: str) -> mqtt.Client:
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"verificacao-{uuid.uuid4().hex[:6]}")
    c.username_pw_set(usuario, senha)
    c.connect(MQTT_HOST, 1883)
    c.loop_start()
    time.sleep(1)
    return c


def main() -> None:
    sims = {d: simulador(d) for d in SENHAS}

    def acionar(device: str, linha: str) -> None:
        sims[device].stdin.write(linha + "\n")
        sims[device].stdin.flush()

    try:
        time.sleep(3)
        token = httpx.post(f"{API}/auth/login", json={"usuario": ENV["ADMIN_USUARIO"], "senha": ENV["ADMIN_SENHA"]}).json()["token"]
        api = httpx.Client(headers={"Authorization": f"Bearer {token}"}, timeout=10)

        def comando(device: str, acao: str, params=None) -> dict:
            r = api.post(f"{API}/comandos", json={"deviceId": device, "acao": acao, "params": params or {}})
            return r.json()

        def aguardar(cmd: dict, alvo: str, limite_s: float = 8) -> dict:
            fim = time.time() + limite_s
            while True:
                cmd = api.get(f"{API}/comandos/{cmd['command_id']}").json()
                if cmd["estado"] == alvo or time.time() > fim:
                    return cmd
                time.sleep(0.3)

        def alertas_ativos() -> set:
            return {a["tipo"] for a in api.get(f"{API}/alertas?ativos=true").json()}

        disp = {d["device_id"]: d for d in api.get(f"{API}/dispositivos").json()}
        checar(disp["porta-01"]["online"] and disp["janela-01"]["online"], "R13: dois dispositivos online, cada um com deviceId")

        r = api.post(f"{API}/comandos", headers={"Authorization": "Bearer invalido"}, json={"deviceId": "porta-01", "acao": "armar"})
        checar(r.status_code == 401, "R9: comando sem autenticação é recusado (401)")

        cmd = aguardar(comando("porta-01", "armar"), "confirmado")
        checar(cmd["estado"] == "confirmado" and all(cmd[k] for k in ("recebido_em", "executado_em", "confirmado_em")),
               "R7: armar passa por enviado -> recebido -> executado -> confirmado")

        cmd = aguardar(comando("janela-01", "disparar"), "rejeitado")
        checar(cmd["erro"] == "E_DESARMADO", f"R7: comando proibido rejeitado com motivo ({cmd['erro']}: {cmd['motivo']})")

        # ACL (R16) e validação (R4): o dispositivo só publica nos próprios tópicos e mensagem inválida é registrada
        intruso = cliente_mqtt("porta-01", SENHAS["porta-01"])
        intruso.publish("access/grupo5/janela-01/telemetria", json.dumps({"v": 1, "deviceId": "janela-01", "ts": None,
                        "uptime_s": 1, "medicoes": [{"grandeza": "presenca", "valor": 1, "unidade": "bool"}]}), qos=1)
        intruso.publish("access/grupo5/porta-01/telemetria", json.dumps({"v": 1, "deviceId": "porta-01", "ts": None,
                        "uptime_s": 1, "medicoes": [{"grandeza": "rssi", "valor": 50, "unidade": "dBm"}]}), qos=1)
        time.sleep(1.5)
        intruso.loop_stop()
        intruso.disconnect()
        presencas = [m for m in api.get(f"{API}/dispositivos/janela-01/medicoes?grandeza=presenca").json() if m["valor"] == 1]
        checar(not presencas, "R16: a ACL impede um dispositivo de publicar no tópico de outro")
        invalida = api.get(f"{API}/mensagens-invalidas?limite=1").json()
        checar(bool(invalida) and invalida[0]["motivo"].startswith("E_VALOR_FORA_FAIXA"),
               f"R4: mensagem fora do contrato descartada e registrada ({invalida[0]['motivo'] if invalida else '-'})")

        # R10 cenário 4: comando duplicado (reentrega QoS 1) não é reexecutado
        backend = cliente_mqtt("access_backend", ENV["MQTT_SENHA_BACKEND"])
        confs = []
        backend.on_message = lambda _c, _u, m: confs.append(json.loads(m.payload))
        backend.subscribe("access/grupo5/janela-01/confirmacao", qos=1)
        time.sleep(0.5)
        payload = json.dumps({"v": 1, "deviceId": "janela-01", "ts": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                              "uptime_s": None, "commandId": str(uuid.uuid4()), "origem": "usuario", "acao": "silenciar",
                              "params": {}, "expira_s": 30})
        for _ in range(2):
            backend.publish("access/grupo5/janela-01/comando", payload, qos=1)
            time.sleep(1)
        backend.loop_stop()
        backend.disconnect()
        checar([c["duplicado"] for c in confs] == [False, False, True, True],
               "R10.4: commandId repetido devolve a confirmação original com duplicado=true")

        # R8: regra remota de escalonamento
        api.put(f"{API}/configuracao", json={"limiteEscalonamento_s": 10})
        aguardar(comando("janela-01", "armar"), "confirmado")
        marco = datetime.now(timezone.utc)
        acionar("porta-01", "abrir")
        time.sleep(16)
        regra = [aguardar(c, "confirmado", 3) for c in api.get(f"{API}/comandos").json()
                 if c["origem"] == "regra" and datetime.fromisoformat(c["enviado_em"]) >= marco]
        checar(sorted(c["device_id"] for c in regra) == ["janela-01", "porta-01"]
               and all(c["estado"] == "confirmado" for c in regra),
               "R8: porta aberta e armada além do limite dispara as sirenes do local (ciclo completo)")
        checar("abertura_armado" in alertas_ativos(), "R6/R15: alerta de invasão aberto (e notificado se NTFY_TOPICO definido)")
        acionar("porta-01", "fechar")
        time.sleep(1.5)
        checar("abertura_armado" not in alertas_ativos(), "R8: alerta encerrado quando a porta fecha")

        # R10 cenário 3: falha de sensor
        aguardar(comando("porta-01", "desarmar"), "confirmado")
        acionar("porta-01", "falha")
        time.sleep(1.5)
        checar("falha_sensor" in alertas_ativos(), "R10.3: falha de sensor vira alerta")
        cmd = aguardar(comando("porta-01", "armar"), "rejeitado")
        checar(cmd["erro"] == "E_SENSOR_FALHA", "R10.3: armar com sensor em falha é rejeitado (E_SENSOR_FALHA)")
        acionar("porta-01", "ok")

        # R10 cenário 2: broker indisponível
        subprocess.run(["docker", "compose", "stop", "mosquitto"], cwd=RAIZ, check=True, capture_output=True)
        time.sleep(2)
        na_queda = comando("porta-01", "silenciar")
        checar(na_queda["estado"] == "sem_resposta", f"R10.2: comando com o broker fora vira sem_resposta ({na_queda['motivo']})")
        subprocess.run(["docker", "compose", "start", "mosquitto"], cwd=RAIZ, check=True, capture_output=True)
        time.sleep(12)
        cmd = aguardar(comando("porta-01", "silenciar"), "confirmado")
        checar(cmd["estado"] == "confirmado", "R10.2: backend e dispositivos reconectam sozinhos e reassinam")
        checar(aguardar(na_queda, "sem_resposta", 0)["estado"] == "sem_resposta", "R10.2: o comando da queda não foi reexecutado")

        a = api.get(f"{API}/analitico/porta-01").json()
        checar(sum(h["eventos"] for h in a["eventos_por_hora"]) >= 1 and a["uptime_pct"] > 0 and len(a["rssi"]) > 0,
               f"R14: indicadores sobre o histórico real (alarme {a['tempo_em_alarme_s']} s, disponibilidade {a['uptime_pct']}%)")

        # R10 cenário 1 (lado do sistema): queda sem aviso -> Last Will publica offline
        sims["janela-01"].kill()
        fim = time.time() + 40
        while time.time() < fim and {d["device_id"]: d for d in api.get(f"{API}/dispositivos").json()}["janela-01"]["online"]:
            time.sleep(1)
        disp = {d["device_id"]: d for d in api.get(f"{API}/dispositivos").json()}
        checar(not disp["janela-01"]["online"], "R10.1: dispositivo que cai sem aviso fica offline (Last Will)")
        api.put(f"{API}/configuracao", json={"limiteEscalonamento_s": 60})
    finally:
        for p in sims.values():
            p.kill()

    print(f"\n{'Tudo certo' if not falhas else f'{falhas} item(ns) falharam'}")
    sys.exit(1 if falhas else 0)


if __name__ == "__main__":
    main()
