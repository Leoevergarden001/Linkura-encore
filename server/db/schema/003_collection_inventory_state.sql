CREATE TABLE IF NOT EXISTS museum_bootstrap_state (
    domain TEXT NOT NULL PRIMARY KEY,
    initialized_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS museum_item_inventory (
    d_item_datas_id TEXT NOT NULL PRIMARY KEY,
    item_num INTEGER NOT NULL CHECK(item_num >= 0),
    FOREIGN KEY(d_item_datas_id) REFERENCES item(d_item_datas_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS museum_music_ownership (
    musics_id INTEGER NOT NULL PRIMARY KEY,
    latest_play_time TEXT NOT NULL DEFAULT '0001-01-01T00:00:00Z',
    FOREIGN KEY(musics_id) REFERENCES collection_music(musics_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS museum_sticker_ownership (
    stickers_id INTEGER NOT NULL PRIMARY KEY,
    requirement_progress INTEGER NOT NULL DEFAULT 0,
    variant_requirement_progress INTEGER NOT NULL DEFAULT 0,
    is_available_variant INTEGER NOT NULL DEFAULT 0 CHECK(is_available_variant IN (0, 1)),
    FOREIGN KEY(stickers_id) REFERENCES collection_sticker(stickers_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS museum_gallary_ownership (
    comics_id INTEGER NOT NULL PRIMARY KEY,
    FOREIGN KEY(comics_id) REFERENCES collection_gallary(comics_id) ON DELETE CASCADE
);
