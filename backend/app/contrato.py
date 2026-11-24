"""Contrato MQTT v1 (CONTRATOS.md): validação das mensagens e montagem dos comandos."""

import json
import re
from datetime import datetime, timezone
from typing import Any

VERSAO = 1
TAMANHO_MAX = 512
PREFIXO = "access/grupo5"
RE_DEVICE_ID = re.compile(r"^[a-z0-9-]{3,24}$")
RE_TS = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
RE_SEMVER = re.compile(r"^\d+\.\d+\.\d+$")
RE_UUID = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
RE_SHA256 = re.compile(r"^[0-9a-f]{64}$")

TIPOS_DISPOSITIVO = ("telemetria", "estado", "confirmacao", "status")

# grandeza -> (unidade, mínimo, máximo)
GRANDEZAS = {
    "abertura": ("bool", 0, 1),
    "presenca": ("bool", 0, 1),
    "aberturas": ("contagem", 0, 1_000_000),
    "rssi": ("dBm", -120, 0),
}

# parâmetro de configurar -> (mínimo, máximo)
CONFIG_FAIXAS = {
    "tempoEntrada_s": (0, 120),
    "intervaloTelemetria_s": (1, 300),
    "timeoutManual_s": (60, 7200),
}

ACOES = ("armar", "desarmar", "disparar", "silenciar", "configurar", "atualizar_firmware")
ORIGENS = ("usuario", "agendamento", "regra")
ERROS = (
    "E_JSON_INVALIDO", "E_VERSAO_NAO_SUPORTADA", "E_CAMPO_AUSENTE", "E_DEVICE_ID_INCORRETO",
    "E_ACAO_DESCONHECIDA", "E_VALOR_FORA_FAIXA", "E_DESARMADO", "E_PORTA_ABERTA",
    "E_SENSOR_FALHA", "E_MODO_MANUAL", "E_EXPIRADO", "E_OTA_FALHA", "E_INTERNO",
)


class ErroContrato(Exception):
    def __init__(self, codigo: str, motivo: str):
        super().__init__(f"{codigo}: {motivo}")
        self.codigo = codigo
        self.motivo = motivo


def _exigir(cond: bool, codigo: str, motivo: str) -> None:
    if not cond:
        raise ErroContrato(codigo, motivo)


def _campo(msg: dict, nome: str, tipo, permitidos=None, nulo=False):
    """Lê um campo obrigatório conferindo tipo (bool não conta como int) e valores permitidos."""
    _exigir(nome in msg, "E_CAMPO_AUSENTE", f"falta o campo {nome}")
    valor = msg[nome]
    if valor is None and nulo:
        return None
    ok = isinstance(valor, tipo) and not (tipo is int and isinstance(valor, bool))
    _exigir(ok, "E_CAMPO_AUSENTE", f"{nome} com tipo errado")
    if permitidos is not None:
        _exigir(valor in permitidos, "E_CAMPO_AUSENTE", f"{nome} = {valor!r} fora de {list(permitidos)}")
    return valor


def _int_faixa(nome: str, valor, minimo: int, maximo: int) -> int:
    _exigir(isinstance(valor, int) and not isinstance(valor, bool), "E_CAMPO_AUSENTE", f"{nome} deve ser inteiro")
    _exigir(minimo <= valor <= maximo, "E_VALOR_FORA_FAIXA", f"{nome} = {valor} fora de {minimo}..{maximo}")
    return valor


def parse_ts(valor: str | None) -> datetime | None:
    if valor is None:
        return None
    _exigir(isinstance(valor, str) and RE_TS.match(valor), "E_CAMPO_AUSENTE", "ts fora do formato ISO 8601 UTC")
    try:
        return datetime.strptime(valor, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    except ValueError:
        raise ErroContrato("E_CAMPO_AUSENTE", "ts com data inválida") from None


def iso(dt: datetime) -> str:
    return dt.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def separar_topico(topico: str) -> tuple[str, str] | None:
    """`access/grupo5/<deviceId>/<tipo>` -> (deviceId, tipo); None se não for tópico de dispositivo."""
    partes = topico.split("/")
    if len(partes) != 4 or "/".join(partes[:2]) != PREFIXO or partes[3] not in TIPOS_DISPOSITIVO:
        return None
    return partes[2], partes[3]


def validar_mensagem(tipo: str, device_id_topico: str, bruto: bytes) -> dict:
    """Valida uma mensagem de dispositivo (seções 3 e 6 do contrato). Devolve o dict ou levanta ErroContrato."""
    _exigir(len(bruto) <= TAMANHO_MAX, "E_JSON_INVALIDO", f"payload com mais de {TAMANHO_MAX} bytes")
    try:
        msg = json.loads(bruto)
    except (ValueError, UnicodeDecodeError):
        raise ErroContrato("E_JSON_INVALIDO", "payload não é JSON") from None
    _exigir(isinstance(msg, dict), "E_JSON_INVALIDO", "payload não é um objeto JSON")

    _exigir("v" in msg, "E_CAMPO_AUSENTE", "falta o campo v")
    _exigir(msg["v"] == VERSAO and not isinstance(msg["v"], bool),
            "E_VERSAO_NAO_SUPORTADA", f"versão {msg['v']!r} não suportada")
    _exigir(RE_DEVICE_ID.match(device_id_topico) is not None, "E_DEVICE_ID_INCORRETO", "deviceId do tópico inválido")
    _exigir(_campo(msg, "deviceId", str) == device_id_topico, "E_DEVICE_ID_INCORRETO", "deviceId difere do tópico")

    # Last Will (status offline) não traz ts nem uptime_s.
    if tipo == "status" and msg.get("status") == "offline":
        msg.setdefault("ts", None)
        msg.setdefault("uptime_s", None)
    else:
        _campo(msg, "ts", str, nulo=True)
        _campo(msg, "uptime_s", int)
        _exigir(msg["uptime_s"] >= 0, "E_VALOR_FORA_FAIXA", "uptime_s negativo")
    msg["_ts"] = parse_ts(msg["ts"])

    {"telemetria": _telemetria, "estado": _estado, "confirmacao": _confirmacao, "status": _status}[tipo](msg)
    return msg


def _telemetria(msg: dict) -> None:
    medicoes = _campo(msg, "medicoes", list)
    _exigir(len(medicoes) > 0, "E_CAMPO_AUSENTE", "medicoes vazia")
    for m in medicoes:
        _exigir(isinstance(m, dict), "E_CAMPO_AUSENTE", "medição não é objeto")
        grandeza = _campo(m, "grandeza", str, GRANDEZAS)
        unidade, minimo, maximo = GRANDEZAS[grandeza]
        _exigir(_campo(m, "unidade", str) == unidade, "E_CAMPO_AUSENTE", f"{grandeza} exige unidade {unidade}")
        _exigir("valor" in m, "E_CAMPO_AUSENTE", f"{grandeza} sem valor")
        _int_faixa(grandeza, m["valor"], minimo, maximo)


def _estado(msg: dict) -> None:
    _exigir(RE_SEMVER.match(_campo(msg, "fw", str)) is not None, "E_CAMPO_AUSENTE", "fw fora de MAJOR.MINOR.PATCH")
    _campo(msg, "modo", str, ("automatico", "manual", "seguro"))
    _campo(msg, "armado", bool)
    _campo(msg, "alarme", str, ("inativo", "entrada", "disparado"))
    _campo(msg, "sensor", str, ("ok", "falha_aberto", "falha_curto", "calibrando"))
    validar_config(_campo(msg, "config", dict), exigir_algum=False)
    ultimo = _campo(msg, "ultimoComandoId", str, nulo=True)
    _exigir(ultimo is None or RE_UUID.match(ultimo) is not None, "E_CAMPO_AUSENTE", "ultimoComandoId não é UUID")


def _confirmacao(msg: dict) -> None:
    _exigir(RE_UUID.match(_campo(msg, "commandId", str)) is not None, "E_CAMPO_AUSENTE", "commandId não é UUID")
    fase = _campo(msg, "fase", str, ("recebido", "executado"))
    _campo(msg, "resultado", str, ("aceito", "rejeitado") if fase == "recebido" else ("ok", "falha"))
    _campo(msg, "erro", str, ERROS, nulo=True)
    _campo(msg, "motivo", str, nulo=True)
    msg["duplicado"] = bool(msg.get("duplicado", False))


def _status(msg: dict) -> None:
    _campo(msg, "status", str, ("online", "offline"))


def validar_config(params: dict, exigir_algum: bool = True) -> None:
    desconhecidos = set(params) - set(CONFIG_FAIXAS)
    _exigir(not desconhecidos, "E_VALOR_FORA_FAIXA", f"parâmetros desconhecidos: {sorted(desconhecidos)}")
    _exigir(params or not exigir_algum, "E_CAMPO_AUSENTE", "configurar exige ao menos um parâmetro")
    for nome, valor in params.items():
        _int_faixa(nome, valor, *CONFIG_FAIXAS[nome])


def validar_comando(acao: Any, params: Any, expira_s: Any) -> None:
    """Confere ação, parâmetros e prazo antes de publicar (seção 4)."""
    _exigir(acao in ACOES, "E_ACAO_DESCONHECIDA", f"ação {acao!r} desconhecida")
    _exigir(isinstance(params, dict), "E_CAMPO_AUSENTE", "params deve ser objeto")
    _int_faixa("expira_s", expira_s, 5, 120)
    if acao == "configurar":
        validar_config(params)
    elif acao == "atualizar_firmware":
        _exigir(set(params) == {"url", "sha256", "versao"}, "E_CAMPO_AUSENTE", "exige url, sha256 e versao")
        url = _campo(params, "url", str)
        _exigir(url.startswith("https://") and len(url) <= 200, "E_VALOR_FORA_FAIXA", "url deve ser HTTPS com até 200 caracteres")
        _exigir(RE_SHA256.match(_campo(params, "sha256", str)) is not None, "E_VALOR_FORA_FAIXA", "sha256 deve ter 64 hexadecimais")
        _exigir(RE_SEMVER.match(_campo(params, "versao", str)) is not None, "E_VALOR_FORA_FAIXA", "versao fora de MAJOR.MINOR.PATCH")
    else:
        _exigir(params == {}, "E_VALOR_FORA_FAIXA", f"{acao} não aceita parâmetros")


def montar_comando(device_id: str, command_id: str, origem: str, acao: str,
                   params: dict, expira_s: int, agora: datetime) -> bytes:
    return json.dumps({
        "v": VERSAO, "deviceId": device_id, "ts": iso(agora), "uptime_s": None,
        "commandId": command_id, "origem": origem, "acao": acao,
        "params": params, "expira_s": expira_s,
    }, separators=(",", ":")).encode()
