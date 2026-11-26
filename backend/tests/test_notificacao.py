"""Testes unitários do cliente de notificação ntfy.sh (R15)."""

import httpx

from app.notificacao import Ntfy


def test_topico_vazio_nao_envia_requisicao(monkeypatch):
    def falhar(*_args, **_kwargs):
        raise AssertionError("httpx.post não deveria ser chamado sem tópico")

    monkeypatch.setattr(httpx, "post", falhar)
    assert Ntfy("https://ntfy.sh", "").enviar("Título", "Mensagem") is False


def test_envio_normal_e_urgente_montam_payload_correto(monkeypatch):
    chamadas = []

    class RespostaOk:
        def raise_for_status(self):
            return None

    def fake_post(url, timeout, json):
        chamadas.append((url, timeout, json))
        return RespostaOk()

    monkeypatch.setattr(httpx, "post", fake_post)

    ntfy = Ntfy("https://ntfy.sh/", "access-grupo5")
    assert ntfy.enviar("Falha de sensor", "Sensor aberto", urgente=False) is True
    assert ntfy.enviar("Invasão: porta-01", "Porta aberta", urgente=True) is True

    assert chamadas[0] == (
        "https://ntfy.sh",
        5,
        {
            "topic": "access-grupo5",
            "title": "Falha de sensor",
            "message": "Sensor aberto",
            "priority": 3,
            "tags": ["warning"],
        },
    )
    assert chamadas[1] == (
        "https://ntfy.sh",
        5,
        {
            "topic": "access-grupo5",
            "title": "Invasão: porta-01",
            "message": "Porta aberta",
            "priority": 5,
            "tags": ["rotating_light"],
        },
    )


def test_falha_http_retorna_false_sem_propagar(monkeypatch):
    def erro_rede(*_args, **_kwargs):
        raise httpx.ConnectError("sem conexão")

    monkeypatch.setattr(httpx, "post", erro_rede)
    assert Ntfy("https://ntfy.sh", "access-grupo5").enviar("Alerta", "Falha") is False


def test_status_erro_http_retorna_false(monkeypatch):
    class RespostaErro:
        def raise_for_status(self):
            req = httpx.Request("POST", "https://ntfy.sh")
            resp = httpx.Response(500, request=req)
            raise httpx.HTTPStatusError("erro interno", request=req, response=resp)

    monkeypatch.setattr(httpx, "post", lambda *_args, **_kwargs: RespostaErro())
    assert Ntfy("https://ntfy.sh", "access-grupo5").enviar("Alerta", "Falha") is False
