-- This is local museum state. The 5.1.0 client request has no idempotency key,
-- so each accepted request is a new draw. A transaction covers the wallet,
-- card ownership, exchange points and history.
CREATE TABLE IF NOT EXISTS museum_gacha_state (
    series_id INTEGER NOT NULL PRIMARY KEY,
    free_jewels INTEGER NOT NULL CHECK(free_jewels >= 0),
    paid_jewels INTEGER NOT NULL CHECK(paid_jewels >= 0),
    exchange_point INTEGER NOT NULL CHECK(exchange_point >= 0),
    draw_count INTEGER NOT NULL CHECK(draw_count >= 0)
);

CREATE TABLE IF NOT EXISTS museum_gacha_history (
    draw_id INTEGER PRIMARY KEY AUTOINCREMENT,
    series_id INTEGER NOT NULL,
    drawn_at TEXT NOT NULL,
    price_type INTEGER NOT NULL,
    price_num INTEGER NOT NULL,
    results_json TEXT NOT NULL
);
