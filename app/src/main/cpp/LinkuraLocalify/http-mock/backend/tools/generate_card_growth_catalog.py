"""Build the card growth catalog used by the offline card upgrade routes.

Card growth in 5.1 (style level, 解放/limit break, 進化/evolution) is driven by
Master tables, all present in the preserved snapshot:

- CardDatas.yaml: rarity, experience type, series and evolve stage per card
  (an evolved card is a separate id: base id + evolve count).
- CardRarities.yaml: style level cap per rarity and evolve stage.
- CardLevels.yaml: cumulative experience per level, i.e. スタイルPt. cost.
- CardLimitBreakMaterials.yaml: material per series and limit break step.
- CardEvolutionMaterials.yaml: up to three materials per evolution target id.

Only base (EvolveTimes 0) cards are listed: the Museum's preserved collection
is already fully developed, so growth applies to base cards from the gacha.
"""
from __future__ import annotations

import json
from collections import defaultdict
from pathlib import Path

import yaml

try:
    LOADER = yaml.CSafeLoader
except AttributeError:
    LOADER = yaml.SafeLoader

TOOLS_DIR = Path(__file__).resolve().parent
MASTER_DIR = TOOLS_DIR / "link-like-diff"
if not (MASTER_DIR / "CardDatas.yaml").exists():
    MASTER_DIR = TOOLS_DIR.parents[8] / "link-like-diff"
BUILTIN_DIR = TOOLS_DIR.parent.parent / "builtin"


def _load(name: str):
    with (MASTER_DIR / name).open(encoding="utf-8") as handle:
        return yaml.load(handle, Loader=LOADER) or []


def generate_json() -> None:
    cards = {row["Id"]: row for row in _load("CardDatas.yaml")}
    caps = {
        str(row["Id"]): [row.get(f"Evolution{stage}_MaxLevel", 0) for stage in range(5)]
        for row in _load("CardRarities.yaml")
    }
    cumulative: dict[int, dict[int, int]] = defaultdict(dict)
    for row in _load("CardLevels.yaml"):
        cumulative[row["ExperienceType"]][row["CardLevel"]] = row["CumulativeExperience"]
    levels = {}
    for exp_type, by_level in cumulative.items():
        top = max(by_level)
        # Index = style level; level 1 costs nothing.
        levels[str(exp_type)] = [0, 0] + [by_level.get(level, 0) for level in range(2, top + 1)]

    limit_break: dict[int, list[dict]] = defaultdict(list)
    for row in _load("CardLimitBreakMaterials.yaml"):
        limit_break[row["CardSeriesId"]].append(
            {"times": row["LimitBreakTimes"], "item_id": row["CostItemsId"], "num": row["CostNum"]})
    evolution = {}
    for row in _load("CardEvolutionMaterials.yaml"):
        costs = [[row[f"CostItemsId{k}"], row[f"CostNum{k}"]]
                 for k in (1, 2, 3) if row.get(f"CostItemsId{k}")]
        evolution[row["Id"]] = costs

    catalog_cards = {}
    for card_id, row in cards.items():
        if row.get("EvolveTimes", 0) != 0:
            continue
        stages = []
        for step in range(1, 5):
            target = card_id + step
            if target not in cards or cards[target].get("CardSeriesId") != row["CardSeriesId"]:
                break
            stages.append(evolution.get(target, []))
        catalog_cards[str(card_id)] = {
            "rarity": row["Rarity"],
            "experience_type": row["ExperienceType"],
            "series_id": row["CardSeriesId"],
            "limit_break": sorted(limit_break.get(row["CardSeriesId"], []), key=lambda r: r["times"]),
            "evolution": stages,
        }

    output = {
        "style_point_item_id": 3004000,
        "rarity_caps": caps,
        "levels": levels,
        "cards": catalog_cards,
    }
    path = BUILTIN_DIR / "card_growth_catalog.json"
    path.write_text(json.dumps(output, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"  Written -> {path} ({len(catalog_cards)} base cards)")


if __name__ == "__main__":
    generate_json()
