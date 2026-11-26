"""Testes unitários de carregamento e validação de configuração (R9)."""

from pathlib import Path

import pytest

from app.config import Config

ENV_BASE = {
    "DATABASE_URL": "postgresql://access:senha@db:5432/access",
    "JWT_SECRET": "segredo-valido-com-mais-de-32-caracteres",
    "ADMIN_USUARIO": "admin",
    "ADMIN_SENHA": "admin123",
    "OPERADOR_USUARIO": "operador",
    "OPERADOR_SENHA": "oper123",
    "MQTT_SENHA_BACKEND": "mqtt123",
}


def test_jwt_secret_curto_levanta_erro():
    with pytest.raises(RuntimeError, match="32 caracteres"):
        Config(
            database_url=ENV_BASE["DATABASE_URL"],
            jwt_secret="curto-demais",
            admin_usuario="admin",
            admin_senha="123",
            operador_usuario="oper",
            operador_senha="123",
        )


def test_do_ambiente_carrega_padroes(monkeypatch):
    for chave in list(ENV_BASE) + ["MQTT_HOST", "MQTT_PORTA", "MQTT_USUARIO", "NTFY_URL", "NTFY_TOPICO", "FIRMWARE_DIR"]:
        monkeypatch.delenv(chave, raising=False)
    for chave, valor in ENV_BASE.items():
        monkeypatch.setenv(chave, valor)

    cfg = Config.do_ambiente()
    assert cfg.mqtt_host == "mosquitto"
    assert cfg.mqtt_porta == 1883
    assert cfg.mqtt_usuario == "access_backend"
    assert cfg.ntfy_url == "https://ntfy.sh"
    assert cfg.ntfy_topico == ""
    assert cfg.fuso == "America/Sao_Paulo"
    assert cfg.firmware_dir == Path("/firmware")


def test_do_ambiente_respeita_variaveis_opcionais(monkeypatch):
    for chave, valor in ENV_BASE.items():
        monkeypatch.setenv(chave, valor)
    monkeypatch.setenv("MQTT_HOST", "127.0.0.1")
    monkeypatch.setenv("MQTT_PORTA", "2883")
    monkeypatch.setenv("MQTT_USUARIO", "custom_backend")
    monkeypatch.setenv("NTFY_URL", "https://ntfy.exemplo.org")
    monkeypatch.setenv("NTFY_TOPICO", "meu-topico")
    monkeypatch.setenv("FIRMWARE_DIR", "/tmp/ota")

    cfg = Config.do_ambiente()
    assert cfg.mqtt_host == "127.0.0.1"
    assert cfg.mqtt_porta == 2883
    assert cfg.mqtt_usuario == "custom_backend"
    assert cfg.ntfy_url == "https://ntfy.exemplo.org"
    assert cfg.ntfy_topico == "meu-topico"
    assert cfg.firmware_dir == Path("/tmp/ota")


@pytest.mark.parametrize("faltando", list(ENV_BASE.keys()))
def test_do_ambiente_exige_variaveis_obrigatorias(monkeypatch, faltando):
    for chave, valor in ENV_BASE.items():
        monkeypatch.setenv(chave, valor)
    monkeypatch.delenv(faltando, raising=False)

    with pytest.raises(RuntimeError, match=faltando):
        Config.do_ambiente()
