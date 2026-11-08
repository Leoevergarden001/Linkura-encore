"""Build the preserved mission catalog and private condition metadata.

Progress and achievement state belong to the local player database.  The
catalog JSON is only a template; the backend fills those fields at request
time.  Keep Master condition types in a separate built-in file so they never
leak into the 5.1.0 API response.
"""
from __future__ import annotations

import json
from pathlib import Path

import yaml


TOOLS_DIR = Path(__file__).resolve().parent
MASTER_DIR = TOOLS_DIR.parents[8] / "link-like-diff"
BUILTIN_DIR = TOOLS_DIR.parent.parent / "builtin"
MISSION_NAMES = {1: "デイリー", 2: "ウィークリー", 3: "メイン"}


def generate_json() -> None:
    missions = yaml.safe_load((MASTER_DIR / "Missions.yaml").read_text(encoding="utf-8"))
    rewards = yaml.safe_load((MASTER_DIR / "MissionRewards.yaml").read_text(encoding="utf-8"))
    rewards_by_mission: dict[int, list[dict]] = {}
    for reward in rewards:
        rewards_by_mission.setdefault(reward["MissionsId"], []).append({
            "reward_type": reward["RewardCategory"],
            "item_type": reward["RewardType"],
            "item_id": reward["ItemsId"],
            "reward_num": reward["RewardNum"],
        })
    groups = []
    for mission_type, series_name in MISSION_NAMES.items():
        rows = sorted(
            (row for row in missions if row.get("MissionType") == mission_type),
            key=lambda row: (row.get("SortOrder", 0), row["Id"]),
        )
        if not rows:
            continue
        groups.append({
            "mission_type": mission_type,
            "series_name": series_name,
            "series_description": "",
            "start_date": min(row["StartTime"] for row in rows).isoformat(),
            "end_date": max(row["EndTime"] for row in rows).isoformat(),
            "mission_list": [{
                "mission_id": row["Id"],
                "name": row["Name"],
                "description": row["Description"],
                "condition_achieve_num": row.get("MissionConditionNum", 0),
                "achieved_num": 0,
                "transition_contents_id": row.get("TransitionContentsId", 0),
                "is_achieved": False,
                "reward_list": rewards_by_mission.get(row["Id"], []),
            } for row in rows],
            # Verified on 5.1.0 (2026-11-08): a non-empty Daily achieve list
            # switches the screen to a Daily+共通-only layout that hides the
            # ウィークリー/メイン tabs, and the Weekly prize panel renders no
            # entries ("5/3"). Keep the prize lists empty so every tab stays
            # usable; prizes are therefore neither shown nor paid offline.
            "achieve_reward_list": [],
        })
    output = {
        "mission_list": groups,
        "common_mission_info": {
            "school_stage_star": 0,
            "rhythm_game_star": 0,
            "received_order": 0,
        },
    }
    output_path = BUILTIN_DIR / "mission_get_list.json"
    output_path.write_text(json.dumps(output, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"  Written -> {output_path}")

    rules = {str(row["Id"]): {
        "mission_type": row["MissionType"],
        "condition_type": row["MissionConditionType"],
        "condition_detail": row.get("MissionConditionDetail", 0),
    } for row in missions if row.get("MissionType") in MISSION_NAMES}
    rules_path = BUILTIN_DIR / "mission_rules.json"
    rules_path.write_text(json.dumps(rules, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"  Written -> {rules_path}")


if __name__ == "__main__":
    generate_json()
