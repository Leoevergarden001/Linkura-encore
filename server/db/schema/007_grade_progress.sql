-- Grade progress of the offline player. Grade Pt is the Grade Pt. item
-- (ItemType 8, 1005000-1005004) earned from rewards; Grade follows the total
-- through GradeDatas. Each grant has its own source key, so it is paid once.
CREATE TABLE IF NOT EXISTS grade_point_ledger (
    source_key TEXT NOT NULL PRIMARY KEY,
    item_id INTEGER NOT NULL,
    points INTEGER NOT NULL CHECK(points >= 0),
    granted_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now'))
);

-- GradeQuestRewards already achieved (each pays out once).
CREATE TABLE IF NOT EXISTS grade_quest_reward_received (
    rewards_id INTEGER NOT NULL PRIMARY KEY,
    received_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now'))
);

-- GradeRewards collected through out_quest_live/grade/set_reward.
CREATE TABLE IF NOT EXISTS grade_reward_received (
    grade_rewards_id INTEGER NOT NULL PRIMARY KEY,
    received_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now'))
);
