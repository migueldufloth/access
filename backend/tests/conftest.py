import json
import os
from pathlib import Path

import psycopg
import pytest
from fastapi.testclient import TestClient

from app.config import Config
from app.main import create_app

URL = os.environ.get("TEST_DATABASE_URL", "postgresql://access:access@localhost:55432/access_test")
_PG_INDISPONIVEL = False


class BrokerFalso:
    """Guarda o que o backend publicaria; `online = False` simula o broker fora do ar."""

    def __init__(self):
        self.publicados, self.online = [], True

    def __call__(self, topico: str, payload: bytes) -> None:
        from app.mqtt import BrokerIndisponivel
        if not self.online:
            raise BrokerIndisponivel("broker MQTT indisponível")
        self.publicados.append((topico, json.loads(payload)))


class NtfyFalso:
    def __init__(self):
        self.enviadas = []

    def enviar(self, titulo: str, mensagem: str, urgente: bool = False) -> bool:
        self.enviadas.append((titulo, mensagem, urgente))
        return True


@pytest.fixture
def ambiente(tmp_path: Path):
    global _PG_INDISPONIVEL
    if _PG_INDISPONIVEL:
        pytest.skip(f"PostgreSQL de teste indisponível em {URL}")
    try:
        with psycopg.connect(URL, autocommit=True, connect_timeout=2) as c:
            c.execute("DROP SCHEMA public CASCADE; CREATE SCHEMA public")
    except psycopg.OperationalError:
        _PG_INDISPONIVEL = True
        pytest.skip(f"PostgreSQL de teste indisponível em {URL}")
    broker, ntfy = BrokerFalso(), NtfyFalso()
    cfg = Config(database_url=URL, jwt_secret="segredo-de-teste-com-32-caracteres!", admin_usuario="admin", admin_senha="admin123",
                 operador_usuario="operador", operador_senha="oper123", firmware_dir=tmp_path)
    with TestClient(create_app(cfg, publicar=broker, notificador=ntfy, ciclo_automatico=False)) as cliente:
        yield cliente, cliente.app.state.servico, broker, ntfy


def token(cliente, usuario="admin", senha="admin123") -> dict:
    r = cliente.post("/api/auth/login", json={"usuario": usuario, "senha": senha})
    assert r.status_code == 200, r.text
    return {"Authorization": f"Bearer {r.json()['token']}"}
