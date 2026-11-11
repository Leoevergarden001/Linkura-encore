-- Runtime player state is deliberately separate from imported catalog records.
-- A Museum profile initially owns each locally catalogued card, but ownership can
-- later be narrowed or populated from a real preserved player save without
-- rewriting the card catalog itself.
CREATE TABLE IF NOT EXISTS museum_card_ownership (
    d_card_datas_id TEXT NOT NULL PRIMARY KEY,
    acquired_at TEXT NOT NULL DEFAULT '2023-04-15T00:00:00Z',
    acquisition_source TEXT NOT NULL DEFAULT 'MUSEUM',
    FOREIGN KEY(d_card_datas_id) REFERENCES card_detail(d_card_datas_id)
);

CREATE TABLE IF NOT EXISTS museum_card_state (
    d_card_datas_id TEXT NOT NULL PRIMARY KEY,
    style_level INTEGER NOT NULL CHECK(style_level >= 0),
    limit_break_times INTEGER NOT NULL CHECK(limit_break_times >= 0),
    is_evolve_max INTEGER NOT NULL DEFAULT 0 CHECK(is_evolve_max IN (0, 1)),
    FOREIGN KEY(d_card_datas_id) REFERENCES museum_card_ownership(d_card_datas_id)
        ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS collection_music (
    musics_id INTEGER NOT NULL PRIMARY KEY,
    response_json TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS collection_sticker (
    stickers_id INTEGER NOT NULL PRIMARY KEY,
    response_json TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS collection_gallary (
    comics_id INTEGER NOT NULL PRIMARY KEY,
    response_json TEXT NOT NULL
);
