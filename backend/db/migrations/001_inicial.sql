-- Esquema inicial do Access (modelo de dados do CP12, seção 7.2).

CREATE TABLE dispositivos (
    device_id      TEXT PRIMARY KEY CHECK (device_id ~ '^[a-z0-9-]{3,24}$'),
    nome           TEXT NOT NULL,
    local          TEXT NOT NULL DEFAULT 'principal',
    fw_versao      TEXT,
    online         BOOLEAN NOT NULL DEFAULT FALSE,
    ultimo_contato TIMESTAMPTZ,
    estado         JSONB,
    aberta_desde   TIMESTAMPTZ,   -- início da abertura contínua atual (regra 1 do R8)
    criado_em      TIMESTAMPTZ NOT NULL DEFAULT now()
);

INSERT INTO dispositivos (device_id, nome) VALUES
    ('porta-01', 'Central da Porta'),
    ('janela-01', 'Satélite de Movimento');

CREATE TABLE medicoes (
    id          BIGSERIAL PRIMARY KEY,
    device_id   TEXT NOT NULL REFERENCES dispositivos,
    grandeza    TEXT NOT NULL,
    valor       DOUBLE PRECISION NOT NULL,
    unidade     TEXT NOT NULL,
    ts          TIMESTAMPTZ NOT NULL,   -- hora do dispositivo, ou do recebimento se ts = null
    recebido_em TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX medicoes_device_grandeza_ts ON medicoes (device_id, grandeza, ts);

CREATE TABLE usuarios (
    id           SERIAL PRIMARY KEY,
    nome_usuario TEXT NOT NULL UNIQUE,
    senha_hash   TEXT NOT NULL,
    perfil       TEXT NOT NULL CHECK (perfil IN ('operador', 'administrador')),
    criado_em    TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE comandos (
    command_id    UUID PRIMARY KEY,
    device_id     TEXT NOT NULL REFERENCES dispositivos,
    origem        TEXT NOT NULL CHECK (origem IN ('usuario', 'agendamento', 'regra')),
    acao          TEXT NOT NULL,
    params        JSONB NOT NULL DEFAULT '{}',
    usuario_id    INTEGER REFERENCES usuarios,
    estado        TEXT NOT NULL CHECK (estado IN
                  ('enviado', 'recebido', 'executado', 'confirmado', 'rejeitado', 'falha', 'sem_resposta')),
    enviado_em    TIMESTAMPTZ NOT NULL DEFAULT now(),
    recebido_em   TIMESTAMPTZ,
    executado_em  TIMESTAMPTZ,
    confirmado_em TIMESTAMPTZ,
    erro          TEXT,
    motivo        TEXT
);
CREATE INDEX comandos_enviado_em ON comandos (enviado_em DESC);

CREATE TABLE alertas (
    id            BIGSERIAL PRIMARY KEY,
    device_id     TEXT NOT NULL REFERENCES dispositivos,
    tipo          TEXT NOT NULL CHECK (tipo IN
                  ('abertura_armado', 'dispositivo_offline', 'falha_sensor', 'agendamento_nao_aplicado')),
    aberto_em     TIMESTAMPTZ NOT NULL DEFAULT now(),
    encerrado_em  TIMESTAMPTZ,
    notificado_em TIMESTAMPTZ,
    detalhe       TEXT
);
-- No máximo um alerta aberto por dispositivo e tipo (um alerta por episódio).
CREATE UNIQUE INDEX alertas_um_aberto ON alertas (device_id, tipo) WHERE encerrado_em IS NULL;

CREATE TABLE eventos_status (
    id        BIGSERIAL PRIMARY KEY,
    device_id TEXT NOT NULL REFERENCES dispositivos,
    status    TEXT NOT NULL CHECK (status IN ('online', 'offline')),
    ts        TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX eventos_status_device_ts ON eventos_status (device_id, ts);

CREATE TABLE agendamentos (
    id             SERIAL PRIMARY KEY,
    device_id      TEXT NOT NULL REFERENCES dispositivos,
    acao           TEXT NOT NULL CHECK (acao IN ('armar', 'desarmar')),
    hora           TIME NOT NULL,
    dias_semana    SMALLINT[] NOT NULL,   -- 0 = segunda ... 6 = domingo
    ativo          BOOLEAN NOT NULL DEFAULT TRUE,
    ultimo_disparo TIMESTAMPTZ
);

CREATE TABLE mensagens_invalidas (
    id          BIGSERIAL PRIMARY KEY,
    topico      TEXT NOT NULL,
    payload     TEXT NOT NULL,
    motivo      TEXT NOT NULL,
    recebido_em TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Parâmetros do sistema alteráveis pela interface.
CREATE TABLE configuracoes (
    chave TEXT PRIMARY KEY,
    valor INTEGER NOT NULL
);
INSERT INTO configuracoes VALUES ('limiteEscalonamento_s', 60);
