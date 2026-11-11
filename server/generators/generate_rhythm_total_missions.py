"""Build the SHOW (rhythm game) total-mission catalog for the offline server.

Source: preserved Master (RhythmGameClassDatas.yaml, RhythmGameTotalMissions.yaml,
RhythmGameTotalMissionRewards.yaml).  The server needs:
- class_goals: per condition type (1 clear count, 2 score accumulated,
  3 high score total), GoalCount indexed by class OrderId, to work out the
  class each class mission has reached;
- total_missions: OrderId, the class OrderId every class mission must reach,
  and the rewards to grant on receive.
"""
from __future__ import annotations

import json
from pathlib import Path

import yaml
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from paths import BUILTIN_DIR, MASTER_DIR

TOOLS_DIR = Path(__file__).resolve().parent
BUILTIN_DIR = BUILTIN_DIR


def _load(name: str):
    return yaml.safe_load((MASTER_DIR / name).read_text(encoding="utf-8")) or []


def build() -> dict:
    goals: dict[str, list[list[int]]] = {}
    for row in _load("RhythmGameClassDatas.yaml"):
        goals.setdefault(str(row["RhythmGameConditionType"]), []).append(
            [int(row["OrderId"]), int(row["GoalCount"])])
    for rows in goals.values():
        rows.sort()
    rewards: dict[int, list[dict]] = {}
    for row in sorted(_load("RhythmGameTotalMissionRewards.yaml"), key=lambda r: (r["RhythmGameTotalMissionsId"], r["OrderId"])):
        rewards.setdefault(row["RhythmGameTotalMissionsId"], []).append(
            {"reward_type": row["RewardType"], "item_id": row["RewardItemId"], "num": row["RewardNum"]})
    missions = [
        {"id": row["Id"], "order_id": row["OrderId"],
         "condition_class_order_id": row["ConditionRhythmGameClassDatasOrderId"],
         "rewards": rewards.get(row["Id"], [])}
        for row in sorted(_load("RhythmGameTotalMissions.yaml"), key=lambda r: r["OrderId"])
    ]
    return {"class_goals": goals, "total_missions": missions}


def generate_json() -> None:
    catalog = build()
    output_path = BUILTIN_DIR / "rhythm_total_missions.json"
    output_path.write_text(json.dumps(catalog, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"  Written -> {output_path} ({len(catalog['total_missions'])} missions)")


if __name__ == "__main__":
    generate_json()
