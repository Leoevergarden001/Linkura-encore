-- Offline Live Grand Prix: the Master's Grand Prix run one after another in
-- real time from the first visit (epoch_day, a 04:00 JST business day). An
-- instance (ordinal) is one run of one Grand Prix; after the last the order
-- starts over. settled_ordinal is the last instance whose rewards were paid.
CREATE TABLE IF NOT EXISTS grand_prix_calendar (
    singleton_id INTEGER NOT NULL PRIMARY KEY CHECK (singleton_id = 1),
    epoch_day INTEGER NOT NULL,
    settled_ordinal INTEGER NOT NULL DEFAULT -1
);

-- One finished live. counted = 0 once the series' daily play limit was used.
CREATE TABLE IF NOT EXISTS grand_prix_play (
    play_id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    ordinal INTEGER NOT NULL,
    grand_prix_id INTEGER NOT NULL,
    business_day INTEGER NOT NULL,
    quest_id INTEGER NOT NULL,
    stage_id INTEGER NOT NULL,
    music_id INTEGER NOT NULL DEFAULT 0,
    score INTEGER NOT NULL,
    point INTEGER NOT NULL,
    counted INTEGER NOT NULL DEFAULT 1 CHECK (counted IN (0, 1)),
    played_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_gp_play_instance ON grand_prix_play(ordinal, business_day, quest_id);

CREATE TABLE IF NOT EXISTS grand_prix_settlement (
    ordinal INTEGER NOT NULL PRIMARY KEY,
    grand_prix_id INTEGER NOT NULL,
    total_point INTEGER NOT NULL,
    personal_rank INTEGER NOT NULL,
    settled_at INTEGER NOT NULL
);
