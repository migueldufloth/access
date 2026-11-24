"""Indicadores do painel analítico (R14), calculados sobre o histórico do banco."""

from datetime import datetime, timedelta


def tempo_ativo_s(eventos: list[tuple[datetime, bool]], inicio: datetime, fim: datetime,
                  ativo_antes: bool = False) -> float:
    """Segundos em que o sinal ficou ativo em [inicio, fim].

    `eventos` são mudanças (instante, ativo) em ordem; `ativo_antes` é o valor vigente em `inicio`.
    Serve para tempo em alarme (alarme disparado) e uptime (status online).
    """
    total, ativo, desde = 0.0, ativo_antes, inicio
    for instante, valor in eventos:
        instante = min(max(instante, inicio), fim)
        if ativo:
            total += (instante - desde).total_seconds()
        ativo, desde = valor, instante
    if ativo:
        total += (fim - desde).total_seconds()
    return total


def eventos_por_hora(leituras: list[tuple[datetime, int]], fim: datetime, horas: int = 24,
                     anterior: int = 0) -> list[dict]:
    """Conta as transições 0 -> 1 (porta abriu, presença detectada) por hora cheia nas últimas `horas`.

    `anterior` é o último valor antes da janela, para não contar uma abertura que já vinha de antes.
    """
    inicio = fim.replace(minute=0, second=0, microsecond=0) - timedelta(hours=horas - 1)
    baldes = {inicio + timedelta(hours=i): 0 for i in range(horas)}
    for instante, valor in leituras:
        hora = instante.replace(minute=0, second=0, microsecond=0)
        if valor == 1 and anterior == 0 and hora in baldes:
            baldes[hora] += 1
        anterior = valor
    return [{"hora": h.isoformat(), "eventos": n} for h, n in baldes.items()]


def media_movel(pontos: list[tuple[datetime, float]], janela: int = 5) -> list[dict]:
    saida = []
    for i, (instante, valor) in enumerate(pontos):
        recorte = [v for _, v in pontos[max(0, i - janela + 1): i + 1]]
        saida.append({"ts": instante.isoformat(), "valor": valor, "media": round(sum(recorte) / len(recorte), 2)})
    return saida
