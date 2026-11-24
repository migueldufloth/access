"""Acesso ao PostgreSQL: pool de conexões e migrações versionadas em db/migrations/."""

import logging
from pathlib import Path

from psycopg.rows import dict_row
from psycopg_pool import ConnectionPool

log = logging.getLogger(__name__)
MIGRACOES = Path(__file__).resolve().parent.parent / "db" / "migrations"


class Banco:
    def __init__(self, url: str):
        self.pool = ConnectionPool(url, min_size=1, max_size=8, open=True,
                                   kwargs={"row_factory": dict_row, "options": "-c timezone=UTC"})

    def todos(self, sql: str, params=None) -> list[dict]:
        with self.pool.connection() as c:
            return c.execute(sql, params).fetchall()

    def um(self, sql: str, params=None) -> dict | None:
        with self.pool.connection() as c:
            return c.execute(sql, params).fetchone()

    def executar(self, sql: str, params=None) -> int:
        with self.pool.connection() as c:
            return c.execute(sql, params).rowcount

    def migrar(self) -> None:
        """Aplica, em ordem e uma única vez, os arquivos NNN_*.sql ainda não aplicados."""
        with self.pool.connection() as c:
            c.execute("SELECT pg_advisory_xact_lock(4242)")   # dois backends subindo juntos não colidem
            c.execute("CREATE TABLE IF NOT EXISTS schema_migrations "
                      "(arquivo TEXT PRIMARY KEY, aplicado_em TIMESTAMPTZ NOT NULL DEFAULT now())")
            feitos = {r["arquivo"] for r in c.execute("SELECT arquivo FROM schema_migrations")}
            for arquivo in sorted(MIGRACOES.glob("*.sql")):
                if arquivo.name not in feitos:
                    log.info("aplicando migração %s", arquivo.name)
                    c.execute(arquivo.read_text())
                    c.execute("INSERT INTO schema_migrations (arquivo) VALUES (%s)", (arquivo.name,))

    def fechar(self) -> None:
        self.pool.close()
