"""Build the read-only mission catalog shown by the offline launcher.

Progress is deliberately zero until a player mission ledger is available. The
catalog itself comes from the preserved 5.1 Master snapshot, not invented UI
rows. Reward claiming must be implemented separately before exposing rewards.
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


if __name__ == "__main__":
    generate_json()
