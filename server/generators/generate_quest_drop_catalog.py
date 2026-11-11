"""Build the stage drop catalog used by the offline Daily and Quest results.

The preserved Master records which materials each Daily and Standard Quest
stage can drop (ItemSources.yaml: DailyQuestStagesId / StandardQuestStagesId).
The per-stage drop tables (DropRewardSeriesId / RandomDropRewardSeriesId)
were server-side and are not in the snapshot, so the quantity is a local
Museum approximation, not the retired server's drop rate:
- Daily: the stage QuestRank (1-5).
- Standard Quest: 1 + QuestLevel // 10 (1-4).
"""
from __future__ import annotations

import json
from pathlib import Path

import yaml
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from paths import BUILTIN_DIR, MASTER_DIR

TOOLS_DIR = Path(__file__).resolve().parent
# Prefer the vendored copy; fall back to the workspace checkout used by the
# other catalog generators (the vendored directory may be an empty placeholder).
BUILTIN_DIR = BUILTIN_DIR


def _load(name: str):
    return yaml.safe_load((MASTER_DIR / name).read_text(encoding="utf-8")) or []


def generate_json() -> None:
    daily = {row["Id"]: row for row in _load("DailyQuestStages.yaml")}
    standard = {row["Id"]: row for row in _load("StandardQuestStages.yaml")}
    drops: dict[int, list[int]] = {}
    for source in _load("ItemSources.yaml"):
        for column, stages in (("DailyQuestStagesId", daily), ("StandardQuestStagesId", standard)):
            for token in str(source.get(column) or "").split(","):
                token = token.strip()
                if token.isdigit() and int(token) in stages:
                    drops.setdefault(int(token), []).append(source["ItemsId"])
    catalog = []
    for stage_id, item_ids in sorted(drops.items()):
        if stage_id in daily:
            quantity = max(1, daily[stage_id].get("QuestRank", 1))
        else:
            quantity = 1 + standard[stage_id].get("QuestLevel", 0) // 10
        catalog.append({"stage_id": stage_id, "quantity": quantity, "item_ids": sorted(set(item_ids))})
    output_path = BUILTIN_DIR / "quest_drop_catalog.json"
    output_path.write_text(json.dumps(catalog, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"  Written -> {output_path} ({len(catalog)} stages)")


if __name__ == "__main__":
    generate_json()
