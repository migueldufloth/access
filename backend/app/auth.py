"""Login com senha bcrypt e token JWT (R9); perfis operador e administrador (R16)."""

from datetime import datetime, timedelta, timezone

import bcrypt
import jwt
from fastapi import Depends, HTTPException, Request, status
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer

VALIDADE = timedelta(hours=8)
_bearer = HTTPBearer(auto_error=False)


def gerar_hash(senha: str) -> str:
    return bcrypt.hashpw(senha.encode(), bcrypt.gensalt()).decode()


def senha_confere(senha: str, hash_: str) -> bool:
    return bcrypt.checkpw(senha.encode(), hash_.encode())


def emitir_token(usuario: dict, segredo: str) -> str:
    agora = datetime.now(timezone.utc)
    return jwt.encode({"sub": str(usuario["id"]), "nome": usuario["nome_usuario"], "perfil": usuario["perfil"],
                       "iat": agora, "exp": agora + VALIDADE}, segredo, algorithm="HS256")


def usuario_atual(request: Request, cred: HTTPAuthorizationCredentials | None = Depends(_bearer)) -> dict:
    if cred is None:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "token ausente")
    try:
        dados = jwt.decode(cred.credentials, request.app.state.config.jwt_secret, algorithms=["HS256"])
    except jwt.PyJWTError:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "token inválido ou expirado") from None
    return {"id": int(dados["sub"]), "nome": dados["nome"], "perfil": dados["perfil"]}


def exigir_admin(usuario: dict = Depends(usuario_atual)) -> dict:
    if usuario["perfil"] != "administrador":
        raise HTTPException(status.HTTP_403_FORBIDDEN, "apenas o administrador envia comandos e altera configurações")
    return usuario
