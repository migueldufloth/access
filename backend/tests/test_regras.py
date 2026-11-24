"""Regras puras: ciclo do comando (R7), decisão remota (R8) e indicadores do painel (R14)."""

from datetime import datetime, timedelta, timezone
from zoneinfo import ZoneInfo

from app import analitico
from app.regras import (Dispositivo, agendamento_vence, alvos_do_disparo, deve_alertar_offline, deve_escalonar,
                        proximo_estado)

AGORA = datetime(2026, 11, 24, 22, 0, tzinfo=timezone.utc)


def disp(device_id="porta-01", local="casa", online=True, contato_s=1, armado=True, alarme="disparado",
         aberta_s=None, alerta_abertura=False, alerta_offline=False):
    return Dispositivo(device_id, local, online, AGORA - timedelta(seconds=contato_s),
                       {"armado": armado, "alarme": alarme, "config": {"intervaloTelemetria_s": 5}},
                       AGORA - timedelta(seconds=aberta_s) if aberta_s is not None else None,
                       alerta_abertura, alerta_offline)


# --- Ciclo do comando ------------------------------------------------------------

def test_comando_avanca_em_ordem():
    assert proximo_estado("enviado", "recebido") == "recebido"
    assert proximo_estado("recebido", "executado") == "executado"
    assert proximo_estado("executado", "confirmado") == "confirmado"


def test_comando_nao_regride_com_reentrega():
    assert proximo_estado("executado", "recebido") is None
    assert proximo_estado("confirmado", "executado") is None


def test_rejeicao_e_falha_sao_finais():
    assert proximo_estado("enviado", "rejeitado") == "rejeitado"
    assert proximo_estado("rejeitado", "recebido") is None
    assert proximo_estado("recebido", "falha") == "falha"


def test_resposta_atrasada_tira_de_sem_resposta():
    assert proximo_estado("sem_resposta", "recebido") == "recebido"


# --- Regra 1: escalonamento ---------------------------------------------------------

def test_escalona_apos_o_limite():
    assert deve_escalonar(disp(aberta_s=61), 60, AGORA)
    assert not deve_escalonar(disp(aberta_s=59), 60, AGORA)


def test_nao_escalona_desarmado_silenciado_ou_repetido():
    assert not deve_escalonar(disp(aberta_s=120, armado=False), 60, AGORA)
    assert not deve_escalonar(disp(aberta_s=120, alarme="inativo"), 60, AGORA)
    assert not deve_escalonar(disp(aberta_s=120, alerta_abertura=True), 60, AGORA)
    assert not deve_escalonar(disp(aberta_s=None), 60, AGORA)


def test_disparo_vai_para_armados_online_do_mesmo_local():
    origem = disp("porta-01")
    todos = [origem, disp("janela-01"), disp("garagem-01", local="outro"),
             disp("sala-01", armado=False), disp("quarto-01", online=False)]
    assert alvos_do_disparo(origem, todos) == ["porta-01", "janela-01"]


# --- Regra 2: offline armado ---------------------------------------------------------

def test_offline_armado_alerta_depois_de_60s():
    assert deve_alertar_offline(disp(online=False, contato_s=61), AGORA)
    assert not deve_alertar_offline(disp(online=False, contato_s=30), AGORA)
    assert deve_alertar_offline(disp(online=True, contato_s=61), AGORA)   # 3 intervalos sem telemetria
    assert not deve_alertar_offline(disp(online=False, contato_s=120, armado=False), AGORA)
    assert not deve_alertar_offline(disp(online=False, contato_s=120, alerta_offline=True), AGORA)


# --- Agendamento ------------------------------------------------------------------

def test_agendamento_dispara_uma_vez_no_minuto():
    sp = ZoneInfo("America/Sao_Paulo")
    terca_22h = datetime(2026, 11, 24, 22, 0, 30, tzinfo=sp)
    assert agendamento_vence((22, 0), [1], None, terca_22h)
    assert not agendamento_vence((22, 0), [1], terca_22h - timedelta(seconds=10), terca_22h)
    assert agendamento_vence((22, 0), [1], terca_22h - timedelta(days=1), terca_22h)
    assert not agendamento_vence((22, 0), [0, 2], None, terca_22h)
    assert not agendamento_vence((22, 1), [1], None, terca_22h)


# --- Analítico --------------------------------------------------------------------

def test_tempo_ativo():
    inicio, fim = AGORA - timedelta(hours=1), AGORA
    eventos = [(inicio + timedelta(minutes=10), True), (inicio + timedelta(minutes=25), False)]
    assert analitico.tempo_ativo_s(eventos, inicio, fim) == 15 * 60
    assert analitico.tempo_ativo_s([], inicio, fim, ativo_antes=True) == 3600


def test_eventos_por_hora_conta_so_transicoes():
    fim = AGORA
    leituras = [(fim - timedelta(minutes=30), 1), (fim - timedelta(minutes=29), 1),
                (fim - timedelta(minutes=20), 0), (fim - timedelta(minutes=10), 1)]
    horas = analitico.eventos_por_hora(leituras, fim, horas=2)
    assert [h["eventos"] for h in horas] == [2, 0]
    assert sum(h["eventos"] for h in analitico.eventos_por_hora(leituras[:1], fim, 2, anterior=1)) == 0


def test_media_movel():
    t = AGORA
    pontos = [(t, -50), (t, -60), (t, -70)]
    assert [p["media"] for p in analitico.media_movel(pontos, janela=2)] == [-50, -55, -65]
