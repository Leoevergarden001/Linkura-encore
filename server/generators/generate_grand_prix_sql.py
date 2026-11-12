"""Live Grand Prix (quest live) static data: every Grand Prix of the Master.

The offline server runs them one after another (see GrandPrixScheduleLocked):
each keeps its original length in days, so the schedule is the Master's in
order, played in real time from the first visit.
"""
from __future__ import annotations

import json
import math
import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
from paths import MASTER_DIR

try:
    YAML_LOADER = yaml.CSafeLoader
except AttributeError:
    YAML_LOADER = yaml.SafeLoader

YAML_DIR = MASTER_DIR
# GrandPrixRewardDatas.RewardType the offline server can pay out: items (1, 3) and
# Grade Pt. (8). Cards (2), card packs (4) and titles (6) are already owned in
# the Museum collection.
PAYABLE_REWARD_TYPES = (1, 3, 8)


def _load_yaml(path: Path):
    with path.open("r", encoding="utf-8") as f:
        return yaml.load(f, Loader=YAML_LOADER) or []


def _iso(value) -> str:
    return str(value).replace("+00:00", "Z").replace(" ", "T")


def _json_sql(value) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).replace("'", "''")


def generate_schema_ddl() -> str:
    lines = [
        "-- Live Grand Prix (quest live), all of GrandPrix.yaml. order_id follows StartTime;",
        "-- duration_days is the original length rounded up to whole days.",
        "CREATE TABLE IF NOT EXISTS grand_prix (",
        "    grand_prix_id INTEGER NOT NULL PRIMARY KEY,",
        "    grand_prix_type INTEGER NOT NULL DEFAULT 1,",
        "    start_date TEXT NOT NULL DEFAULT '',",
        "    end_date TEXT NOT NULL DEFAULT '',",
        "    rewards_json TEXT NOT NULL DEFAULT '[]',",
        "    order_id INTEGER NOT NULL DEFAULT 0,",
        "    duration_days INTEGER NOT NULL DEFAULT 1",
        ");",
        "",
        "CREATE TABLE IF NOT EXISTS grand_prix_series (",
        "    quest_id INTEGER NOT NULL PRIMARY KEY,",
        "    grand_prix_id INTEGER NOT NULL,",
        "    order_id INTEGER NOT NULL DEFAULT 1,",
        "    play_limit_count INTEGER NOT NULL DEFAULT 0,",
        "    retire_limit_count INTEGER NOT NULL DEFAULT 0",
        ");",
        "",
        "CREATE INDEX IF NOT EXISTS idx_gp_series_gp ON grand_prix_series(grand_prix_id);",
        "",
        "-- score1..3: the stage rank thresholds; bonus0..3: GP Pt. bonus per rank (1/100 %).",
        "CREATE TABLE IF NOT EXISTS grand_prix_stage (",
        "    stage_id INTEGER NOT NULL PRIMARY KEY,",
        "    quest_id INTEGER NOT NULL,",
        "    order_id INTEGER NOT NULL DEFAULT 1,",
        "    quest_musics_type INTEGER NOT NULL DEFAULT 0,",
        "    quest_musics_detail INTEGER NOT NULL DEFAULT 0,",
        "    release_condition_type INTEGER NOT NULL DEFAULT 0,",
        "    release_condition_value INTEGER NOT NULL DEFAULT 0,",
        "    section_skills_json TEXT NOT NULL DEFAULT '[]',",
        "    score1 INTEGER NOT NULL DEFAULT 0,",
        "    score2 INTEGER NOT NULL DEFAULT 0,",
        "    score3 INTEGER NOT NULL DEFAULT 0,",
        "    bonus0 INTEGER NOT NULL DEFAULT 10000,",
        "    bonus1 INTEGER NOT NULL DEFAULT 10000,",
        "    bonus2 INTEGER NOT NULL DEFAULT 10000,",
        "    bonus3 INTEGER NOT NULL DEFAULT 10000",
        ");",
        "",
        "CREATE INDEX IF NOT EXISTS idx_gp_stage_quest ON grand_prix_stage(quest_id);",
        "",
        "-- GrandPrixPointBonuses: target_type 1 = card rarity (target_detail) at",
        "-- limit break target_num; target_type 2 = character (not applied offline).",
        "CREATE TABLE IF NOT EXISTS grand_prix_point_bonus (",
        "    grand_prix_id INTEGER NOT NULL,",
        "    target_type INTEGER NOT NULL,",
        "    target_detail INTEGER NOT NULL,",
        "    target_num INTEGER NOT NULL,",
        "    bonus_value INTEGER NOT NULL,",
        "    PRIMARY KEY (grand_prix_id, target_type, target_detail, target_num)",
        ");",
        "",
        "-- GrandPrixRewards + payable GrandPrixRewardDatas, by personal (or circle) rank.",
        "CREATE TABLE IF NOT EXISTS grand_prix_reward (",
        "    rewards_id INTEGER NOT NULL PRIMARY KEY,",
        "    grand_prix_id INTEGER NOT NULL,",
        "    min_rank INTEGER NOT NULL,",
        "    max_rank INTEGER NOT NULL,",
        "    rewards_json TEXT NOT NULL DEFAULT '[]'",
        ");",
        "",
        "CREATE INDEX IF NOT EXISTS idx_gp_reward_gp ON grand_prix_reward(grand_prix_id);",
        "",
        "-- CardDatas.Rarity, for the Grand Prix card bonus.",
        "CREATE TABLE IF NOT EXISTS card_rarity (",
        "    card_datas_id INTEGER NOT NULL PRIMARY KEY,",
        "    rarity INTEGER NOT NULL",
        ");",
        "",
        "CREATE TABLE IF NOT EXISTS grand_prix_progress (",
        "    stage_id INTEGER NOT NULL PRIMARY KEY,",
        "    high_score INTEGER NOT NULL DEFAULT 0,",
        "    daily_best_score INTEGER NOT NULL DEFAULT 0,",
        "    play_count INTEGER NOT NULL DEFAULT 0",
        ");",
        "",
    ]
    return "\n".join(lines)


def generate_seed_statements() -> list[str]:
    grand_prix_list = sorted(_load_yaml(YAML_DIR / "GrandPrix.yaml"), key=lambda g: (g["StartTime"], g["Id"]))
    series_list = _load_yaml(YAML_DIR / "GrandPrixQuestSeries.yaml")
    stages_list = _load_yaml(YAML_DIR / "GrandPrixQuestStages.yaml")
    release_map = {rc["ReleaseGrandPrixId"]: rc for rc in _load_yaml(YAML_DIR / "GrandPrixReleaseCondition.yaml")}
    rewards = _load_yaml(YAML_DIR / "GrandPrixRewards.yaml")
    reward_datas = _load_yaml(YAML_DIR / "GrandPrixRewardDatas.yaml")
    bonuses = _load_yaml(YAML_DIR / "GrandPrixPointBonuses.yaml")

    datas_by_reward: dict[int, list[dict]] = {}
    for rd in sorted(reward_datas, key=lambda r: r["Id"]):
        datas_by_reward.setdefault(rd["GrandPrixRewardsId"], []).append(rd)

    section_map: dict[int, list[dict]] = {}
    for sec in _load_yaml(YAML_DIR / "QuestSections.yaml"):
        section_map.setdefault(sec["QuestStagesId"], []).append(
            {"section_no": sec["SectionNo"], "section_skills_id": sec["SectionSkillsId"]})

    gp_ids = {gp["Id"] for gp in grand_prix_list}
    statements: list[str] = []
    for order, gp in enumerate(grand_prix_list, start=1):
        gp_id = gp["Id"]
        days = max(1, math.ceil((gp["EndTime"] - gp["StartTime"]).total_seconds() / 86400))
        display = []
        for r in rewards:
            if r["GrandPrixesId"] != gp_id or r.get("IsDisplay", 1) == 0:
                continue
            display.append({
                "grand_prix_rewards_id": r["Id"],
                "min_target_num": r["MinTargetNum"],
                "max_target_num": r["MaxTargetNum"],
                "reward_datas": [{"grand_prix_reward_datas_id": d["Id"], "item_type": d["RewardType"],
                                  "item_id": d["RewardItemId"], "num": d["RewardNum"]}
                                 for d in datas_by_reward.get(r["Id"], [])],
            })
        statements.append(
            "INSERT OR IGNORE INTO grand_prix (grand_prix_id, grand_prix_type, start_date, end_date, rewards_json, "
            "order_id, duration_days) "
            f"VALUES ({gp_id}, {gp['GrandPrixType']}, '{_iso(gp['StartTime'])}', '{_iso(gp['EndTime'])}', "
            f"'{_json_sql(display)}', {order}, {days});")

    series_ids = set()
    for sr in series_list:
        if sr["GrandPrixesId"] not in gp_ids:
            continue
        series_ids.add(sr["Id"])
        statements.append(
            "INSERT OR IGNORE INTO grand_prix_series (quest_id, grand_prix_id, order_id, play_limit_count, "
            "retire_limit_count) "
            f"VALUES ({sr['Id']}, {sr['GrandPrixesId']}, {sr['OrderId']}, {sr['PlayLimitCount']}, "
            f"{sr['RetireLimitCount']});")

    for st in stages_list:
        if st["GrandPrixSeriesId"] not in series_ids:
            continue
        rc = release_map.get(st["Id"], {"ConditionsType": 0, "ConditionsValue": 0})
        sections = sorted(section_map.get(st["Id"], []), key=lambda x: x["section_no"])
        statements.append(
            "INSERT OR IGNORE INTO grand_prix_stage (stage_id, quest_id, order_id, quest_musics_type, "
            "quest_musics_detail, release_condition_type, release_condition_value, section_skills_json, "
            "score1, score2, score3, bonus0, bonus1, bonus2, bonus3) "
            f"VALUES ({st['Id']}, {st['GrandPrixSeriesId']}, {st['OrderId']}, {st['QuestMusicsType']}, "
            f"{st['QuestMusicsDetail']}, {rc['ConditionsType']}, {rc['ConditionsValue']}, '{_json_sql(sections)}', "
            f"{st['Score1']}, {st['Score2']}, {st['Score3']}, {st['ScoreBonusValue0']}, {st['ScoreBonusValue1']}, "
            f"{st['ScoreBonusValue2']}, {st['ScoreBonusValue3']});")

    for b in bonuses:
        if b["GrandPrixesId"] not in gp_ids:
            continue
        statements.append(
            "INSERT OR IGNORE INTO grand_prix_point_bonus (grand_prix_id, target_type, target_detail, target_num, "
            f"bonus_value) VALUES ({b['GrandPrixesId']}, {b['TargetType']}, {b['TargetDetail']}, "
            f"{b['TargetNum']}, {b['BonusValue']});")

    for r in rewards:
        if r["GrandPrixesId"] not in gp_ids:
            continue
        payable = [{"reward_type": d["RewardType"], "item_id": d["RewardItemId"], "num": d["RewardNum"]}
                   for d in datas_by_reward.get(r["Id"], []) if d["RewardType"] in PAYABLE_REWARD_TYPES]
        statements.append(
            "INSERT OR IGNORE INTO grand_prix_reward (rewards_id, grand_prix_id, min_rank, max_rank, rewards_json) "
            f"VALUES ({r['Id']}, {r['GrandPrixesId']}, {r['MinTargetNum']}, {r['MaxTargetNum']}, "
            f"'{_json_sql(payable)}');")

    for card in _load_yaml(YAML_DIR / "CardDatas.yaml"):
        statements.append(
            f"INSERT OR IGNORE INTO card_rarity (card_datas_id, rarity) VALUES ({card['Id']}, {card['Rarity']});")
    return statements


if __name__ == "__main__":
    stmts = generate_seed_statements()
    print(f"Generated {len(stmts)} INSERT statements")
