CREATE TABLE IF NOT EXISTS rhythm_runtime_session (
    singleton_id INTEGER NOT NULL PRIMARY KEY CHECK (singleton_id = 1),
    session_id TEXT NOT NULL,
    music_id INTEGER NOT NULL,
    difficulty INTEGER NOT NULL,
    state TEXT NOT NULL CHECK (state IN ('PLAYING', 'FINISHED', 'RETIRED')),
    start_payload_json TEXT NOT NULL CHECK (json_valid(start_payload_json)),
    finish_payload_json TEXT CHECK (
        finish_payload_json IS NULL OR json_valid(finish_payload_json)
    ),
    result_json TEXT CHECK (result_json IS NULL OR json_valid(result_json)),
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
