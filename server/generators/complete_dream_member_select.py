"""Add Dream Live series missing from the captured get_quest_top.

builtin/out_quest_live_get_quest_top.json was captured before the 102期
Oracle Étude (series list 1003) quests existed, so dream_live_member_select
lacks 61021803 / 61022803 / 61023803 and the client leaves those three
member tiles empty.  For every DreamQuestSeries in the preserved Master that
the capture lacks, add an entry shaped like a captured one of the same
series list: first stage (series*10), the member's centre-song list from
their latest captured series, and the release conditions of a captured
sibling with the member name substituted.  Existing entries are untouched.
"""
from __future__ import annotations

import json
import re
from pathlib import Path

import yaml
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from paths import BUILTIN_DIR, MASTER_DIR

TOOLS_DIR = Path(__file__).resolve().parent
TARGET = BUILTIN_DIR / "out_quest_live_get_quest_top.json"


def _load(name: str):
    return yaml.safe_load((MASTER_DIR / name).read_text(encoding="utf-8")) or []


def complete(top: dict) -> list[int]:
    series = {row["Id"]: row for row in _load("DreamQuestSeries.yaml")}
    stages = {row["Id"] for row in _load("DreamQuestStages.yaml")}
    entries = top["dream_live_member_select"]
    by_id = {e["dream_quest_series_id"]: e for e in entries}
    added = []
    for series_id, row in sorted(series.items()):
        if series_id in by_id:
            continue
        list_id = row["DreamLiveSeriesListId"]
        sibling = next((e for e in entries if e.get("dream_live_series_id") == list_id), None)
        own = [e for e in entries if series.get(e["dream_quest_series_id"], {}).get("CharactersId") == row["CharactersId"]]
        stage_id = series_id * 10
        if sibling is None or not own or stage_id not in stages:
            raise SystemExit(f"cannot build {series_id}: no template")
        sibling_name = series[sibling["dream_quest_series_id"]]["Name"]
        name = row["Name"]
        conditions = json.loads(json.dumps(sibling["condition_list"], ensure_ascii=False).replace(sibling_name, name))
        entry = {
            "dream_quest_series_id": series_id,
            "dream_quest_stages_id": stage_id,
            "clear_times": 0,
            "is_clear": False,
            "is_get_card": True,
            "limit_break_times": 0,
            "condition_list": conditions,
            "music_list": max(own, key=lambda e: e["dream_quest_series_id"])["music_list"],
            "dream_live_series_id": list_id,
        }
        entries.append(entry)
        added.append(series_id)
    return added


def main() -> None:
    text = TARGET.read_text(encoding="utf-8")
    top = json.loads(text)
    added = complete(top)
    second = text.split("\n", 2)[1] if "\n" in text else ""
    indent = (len(second) - len(second.lstrip(" "))) or None
    TARGET.write_text(json.dumps(top, ensure_ascii=False, indent=indent) + "\n", encoding="utf-8")
    print(f"  {TARGET.name}: added {added}")


if __name__ == "__main__":
    main()
