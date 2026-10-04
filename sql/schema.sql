-- tickstream schema.
--
-- price and quantity are NUMERIC: Binance sends exact decimal strings and
-- NUMERIC keeps them exact (a float would round them).
--
-- trade_id is unique per symbol, not globally, so the key is the pair. With
-- INSERT ... ON CONFLICT DO NOTHING this also makes writes idempotent:
-- replaying a capture or overlapping a reconnect can't create duplicates.

CREATE TABLE IF NOT EXISTS trades (
    symbol          TEXT        NOT NULL,
    trade_id        BIGINT      NOT NULL,
    price           NUMERIC     NOT NULL,   -- exact decimal, parsed from the string
    quantity        NUMERIC     NOT NULL,
    event_time      TIMESTAMPTZ NOT NULL,   -- Binance "T" (exchange trade time)
    received_at     TIMESTAMPTZ NOT NULL,   -- our wall clock when the message arrived
    is_buyer_maker  BOOLEAN     NOT NULL,
    PRIMARY KEY (symbol, trade_id)
);
