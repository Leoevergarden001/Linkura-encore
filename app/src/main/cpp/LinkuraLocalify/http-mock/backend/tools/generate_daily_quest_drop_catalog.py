"""Build the Daily Live drop catalog used by the offline result handler.

The preserved Master records which materials each Daily stage can drop
(ItemSources.yaml) and each stage's QuestRank (DailyQuestStages.yaml). The
per-stage drop tables (DropRewardSeriesId) were server-side and are not in the
Master snapshot, so quantities are a local Museum approximation derived from
QuestRank. They are not the retired server's drop rates.
"""
from __future__ import annotations

import json
from pathlib import Path

import yaml

TOOLS_DIR = Path(__file__).resolve().parent
# Prefer the vendored copy; fall back to the workspace checkout used by the
# other catalog generators (the vendored directory may be an empty placeholder).
MASTER_DIR = TOOLS_DIR / "link-like-diff"
if not (MASTER_DIR / "ItemSources.yaml").exists():
    MASTER_DIR = TOOLS_DIR.parents[8] / "link-like-diff"
BUILTIN_DIR = TOOLS_DIR.parent.parent / "builtin"


def _load(name: str):
    return yaml.safe_load((MASTER_DIR / name).read_text(encoding="utf-8")) or []


def generate_json() -> None:
    stages = {row["Id"]: row for row in _load("DailyQuestStages.yaml")}
    drops: dict[int, list[int]] = {}
    for source in _load("ItemSources.yaml"):
        raw = source.get("DailyQuestStagesId") or ""
        for token in str(raw).split(","):
            token = token.strip()
            if token.isdigit() and int(token) in stages:
                drops.setdefault(int(token), []).append(source["ItemsId"])
    catalog = [{
        "stage_id": stage_id,
        "quest_rank": stages[stage_id].get("QuestRank", 1),
        "item_ids": sorted(item_ids),
    } for stage_id, item_ids in sorted(drops.items())]
    output_path = BUILTIN_DIR / "daily_quest_drop_catalog.json"
    output_path.write_text(json.dumps(catalog, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"  Written -> {output_path} ({len(catalog)} stages)")


if __name__ == "__main__":
    generate_json()
