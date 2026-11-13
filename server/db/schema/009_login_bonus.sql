-- Daily login bonus of the offline player (LoginBonuses 101001, 7-day loop).
-- last_day is the 04:00 JST business day of the last stamp, day_count the
-- stamp reached that day (1..7).
CREATE TABLE IF NOT EXISTS offline_login_bonus (
    singleton_id INTEGER NOT NULL PRIMARY KEY CHECK (singleton_id = 1),
    last_day INTEGER NOT NULL,
    day_count INTEGER NOT NULL CHECK (day_count BETWEEN 1 AND 7)
);
