-- Player actions are recorded once by their source operation.  Periods are
-- derived at read time using the game's 04:00 JST daily boundary.
CREATE TABLE IF NOT EXISTS offline_mission_event (
    source_key TEXT NOT NULL PRIMARY KEY,
    event_kind TEXT NOT NULL,
    amount INTEGER NOT NULL DEFAULT 1 CHECK(amount >= 0),
    detail INTEGER NOT NULL DEFAULT 0,
    occurred_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now'))
);
CREATE INDEX IF NOT EXISTS idx_offline_mission_event_kind_time
    ON offline_mission_event(event_kind, occurred_at);

-- Reserved for one-time reward settlement.  Existing Museum progress must
-- first be baselined before rewards can be issued.
CREATE TABLE IF NOT EXISTS offline_mission_award (
    period_key TEXT NOT NULL,
    mission_id INTEGER NOT NULL,
    award_state TEXT NOT NULL CHECK(award_state IN ('BASELINE', 'GRANTED')),
    PRIMARY KEY(period_key, mission_id)
);

CREATE TABLE IF NOT EXISTS offline_mission_meta (
    key TEXT NOT NULL PRIMARY KEY,
    value TEXT NOT NULL
);
