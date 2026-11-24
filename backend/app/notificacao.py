"""Notificação externa por push no ntfy.sh (R15)."""

import logging

import httpx

log = logging.getLogger(__name__)


class Ntfy:
    def __init__(self, url: str, topico: str):
        self.url, self.topico = url.rstrip("/"), topico

    def enviar(self, titulo: str, mensagem: str, urgente: bool = False) -> bool:
        if not self.topico:
            return False
        try:
            # Envio em JSON na raiz do servidor: aceita acentos no título (cabeçalho HTTP não aceita).
            r = httpx.post(self.url, timeout=5, json={
                "topic": self.topico, "title": titulo, "message": mensagem,
                "priority": 5 if urgente else 3, "tags": ["rotating_light" if urgente else "warning"],
            })
            r.raise_for_status()
            return True
        except httpx.HTTPError as e:
            log.warning("falha ao notificar no ntfy: %s", e)
            return False
