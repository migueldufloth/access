"""Testes unitários de autenticação bcrypt, JWT e perfis (R9 e R16)."""

from datetime import datetime, timedelta, timezone
from types import SimpleNamespace

import jwt
import pytest
from fastapi import HTTPException
from fastapi.security import HTTPAuthorizationCredentials

from app import auth

SEGREDO = "segredo-de-teste-com-32-caracteres!"


def _request_com_segredo(segredo: str = SEGREDO):
    return SimpleNamespace(app=SimpleNamespace(state=SimpleNamespace(config=SimpleNamespace(jwt_secret=segredo))))


def test_hash_e_verificacao_de_senha():
    hashed = auth.gerar_hash("minha-senha-forte")
    assert hashed != "minha-senha-forte"
    assert auth.senha_confere("minha-senha-forte", hashed) is True
    assert auth.senha_confere("senha-errada", hashed) is False


def test_emitir_e_decodificar_token_valido():
    usuario = {"id": 7, "nome_usuario": "admin", "perfil": "administrador"}
    tok = auth.emitir_token(usuario, SEGREDO)
    cred = HTTPAuthorizationCredentials(scheme="Bearer", credentials=tok)

    atual = auth.usuario_atual(_request_com_segredo(), cred)
    assert atual == {"id": 7, "nome": "admin", "perfil": "administrador"}


def test_usuario_atual_recusa_ausente_invalido_ou_expirado():
    req = _request_com_segredo()

    with pytest.raises(HTTPException) as exc:
        auth.usuario_atual(req, None)
    assert exc.value.status_code == 401

    tok_outro_segredo = auth.emitir_token({"id": 1, "nome_usuario": "admin", "perfil": "administrador"}, "x" * 32)
    with pytest.raises(HTTPException) as exc:
        auth.usuario_atual(req, HTTPAuthorizationCredentials(scheme="Bearer", credentials=tok_outro_segredo))
    assert exc.value.status_code == 401

    expirado = jwt.encode(
        {
            "sub": "1",
            "nome": "admin",
            "perfil": "administrador",
            "iat": datetime.now(timezone.utc) - timedelta(hours=10),
            "exp": datetime.now(timezone.utc) - timedelta(hours=2),
        },
        SEGREDO,
        algorithm="HS256",
    )
    with pytest.raises(HTTPException) as exc:
        auth.usuario_atual(req, HTTPAuthorizationCredentials(scheme="Bearer", credentials=expirado))
    assert exc.value.status_code == 401


def test_exigir_admin_permite_admin_e_bloqueia_operador():
    admin = {"id": 1, "nome": "admin", "perfil": "administrador"}
    operador = {"id": 2, "nome": "operador", "perfil": "operador"}

    assert auth.exigir_admin(admin) == admin
    with pytest.raises(HTTPException) as exc:
        auth.exigir_admin(operador)
    assert exc.value.status_code == 403
