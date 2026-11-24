"""Configuração lida do ambiente (.env no compose). Nenhum segredo fica no código."""

import os
from dataclasses import dataclass
from pathlib import Path


def _obrigatoria(nome: str) -> str:
    valor = os.environ.get(nome)
    if not valor:
        raise RuntimeError(f"variável de ambiente {nome} não definida (ver .env.example)")
    return valor


@dataclass(frozen=True)
class Config:
    database_url: str
    jwt_secret: str
    admin_usuario: str
    admin_senha: str
    operador_usuario: str
    operador_senha: str
    mqtt_host: str = "mosquitto"
    mqtt_porta: int = 1883
    mqtt_usuario: str = "access_backend"
    mqtt_senha: str = ""
    ntfy_url: str = "https://ntfy.sh"
    ntfy_topico: str = ""          # vazio desliga a notificação externa
    fuso: str = "America/Sao_Paulo"
    firmware_dir: Path = Path("/firmware")

    def __post_init__(self):
        if len(self.jwt_secret) < 32:
            raise RuntimeError("JWT_SECRET precisa de 32 caracteres ou mais")

    @classmethod
    def do_ambiente(cls) -> "Config":
        return cls(
            database_url=_obrigatoria("DATABASE_URL"),
            jwt_secret=_obrigatoria("JWT_SECRET"),
            admin_usuario=_obrigatoria("ADMIN_USUARIO"),
            admin_senha=_obrigatoria("ADMIN_SENHA"),
            operador_usuario=_obrigatoria("OPERADOR_USUARIO"),
            operador_senha=_obrigatoria("OPERADOR_SENHA"),
            mqtt_host=os.environ.get("MQTT_HOST", "mosquitto"),
            mqtt_porta=int(os.environ.get("MQTT_PORTA", "1883")),
            mqtt_usuario=os.environ.get("MQTT_USUARIO", "access_backend"),
            mqtt_senha=_obrigatoria("MQTT_SENHA_BACKEND"),
            ntfy_url=os.environ.get("NTFY_URL", "https://ntfy.sh"),
            ntfy_topico=os.environ.get("NTFY_TOPICO", ""),
            firmware_dir=Path(os.environ.get("FIRMWARE_DIR", "/firmware")),
        )
