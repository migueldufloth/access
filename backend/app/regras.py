"""Lógica pura (sem banco nem rede): ciclo do comando (R7) e decisão remota (R8, Anexo A do CP12)."""

from dataclasses import dataclass
from datetime import datetime, timedelta

# --- Ciclo do comando -------------------------------------------------------

_ORDEM = {"enviado": 0, "sem_resposta": 0, "recebido": 1, "executado": 2, "confirmado": 3}
FINAIS = {"confirmado", "rejeitado", "falha"}
TIMEOUT_RESPOSTA_S = 10


def proximo_estado(atual: str, evento: str) -> str | None:
    """Novo estado do comando para o evento recebido, ou None se não muda.

    Eventos: recebido, rejeitado, executado, falha, confirmado. O estado só avança
    (reentrega QoS 1 ou confirmação duplicada não regride) e para nos estados finais.
    Uma resposta atrasada tira o comando de sem_resposta.
    """
    if atual in FINAIS:
        return None
    if evento in ("rejeitado", "falha"):
        return evento
    return evento if _ORDEM[evento] > _ORDEM[atual] else None


def evento_da_confirmacao(fase: str, resultado: str) -> str:
    return {("recebido", "aceito"): "recebido", ("recebido", "rejeitado"): "rejeitado",
            ("executado", "ok"): "executado", ("executado", "falha"): "falha"}[(fase, resultado)]


# --- Decisão remota ---------------------------------------------------------

LIMITE_OFFLINE_S = 60


@dataclass(frozen=True)
class Dispositivo:
    device_id: str
    local: str
    online: bool
    ultimo_contato: datetime | None
    estado: dict | None
    aberta_desde: datetime | None
    alerta_abertura: bool   # já existe alerta abertura_armado aberto
    alerta_offline: bool    # já existe alerta dispositivo_offline aberto

    @property
    def armado(self) -> bool:
        return bool(self.estado and self.estado.get("armado"))

    @property
    def intervalo_s(self) -> int:
        return ((self.estado or {}).get("config") or {}).get("intervaloTelemetria_s", 5)


def deve_escalonar(d: Dispositivo, limite_s: int, agora: datetime) -> bool:
    """Regra 1: armado, porta aberta há mais que o limite e alarme ainda soando (não silenciado)."""
    return (d.armado and not d.alerta_abertura and d.aberta_desde is not None
            and (d.estado or {}).get("alarme") in ("entrada", "disparado")
            and agora - d.aberta_desde > timedelta(seconds=limite_s))


def deve_alertar_offline(d: Dispositivo, agora: datetime) -> bool:
    """Regra 2: armado e sem contato (offline ou 3 intervalos sem telemetria) há mais de 60 s."""
    if not d.armado or d.alerta_offline or d.ultimo_contato is None:
        return False
    silencio = agora - d.ultimo_contato
    sumiu = not d.online or silencio > timedelta(seconds=3 * d.intervalo_s)
    return sumiu and silencio > timedelta(seconds=LIMITE_OFFLINE_S)


def alvos_do_disparo(origem: Dispositivo, todos: list[Dispositivo]) -> list[str]:
    """Regra 1, efeito 3: todos os dispositivos armados e online do mesmo local."""
    return [d.device_id for d in todos if d.local == origem.local and d.armado and d.online]


def agendamento_vence(hora_minuto: tuple[int, int], dias_semana: list[int],
                      ultimo_disparo: datetime | None, agora_local: datetime) -> bool:
    """O agendamento dispara uma vez no minuto exato (hora local), nos dias marcados (0 = segunda)."""
    if agora_local.weekday() not in dias_semana or (agora_local.hour, agora_local.minute) != hora_minuto:
        return False
    minuto = agora_local.replace(second=0, microsecond=0)
    return ultimo_disparo is None or ultimo_disparo < minuto
