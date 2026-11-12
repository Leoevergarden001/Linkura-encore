"""Generate the offline Gacha's standard-card candidate catalog.

Input is the preserved R2606000 5.1.0 Master snapshot in link-like-diff.
The snapshot identifies eligible cards, but does not contain the retired
server's per-card draw probabilities.
"""

from __future__ import annotations

import argparse
import calendar
import hashlib
import json
from pathlib import Path

import yaml
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from paths import MASTER_DIR, SERVER_DIR

DEFAULT_MASTER = MASTER_DIR
OUTPUT = SERVER_DIR / "src" / "backend" / "MuseumGachaCatalog.hpp"
# Series that ended before the service closed and are opened offline by
# patching their Master EndTime (tools/linkura_offline master-open-gacha).
OPENED_SERIES_OUTPUT = SERVER_DIR / "data" / "museum_gacha_opened_series.json"
POOL_IDS = (1055001, 1055002, 1055003)
# The seven series still open when the service closed (Master OrderId 1-11).
CLOSING_POOL_IDS = (1050505, 1050506, 1050509, 1050511, *POOL_IDS)
# Birthday series follow them: OrderId 101.. in StartTime order.
BIRTHDAY_ORDER_BASE = 101
# Master EndTime of the series open at the end of service (2035-12-31 14:59:59 UTC, .NET ticks).
OPEN_END_TICKS = 642183227990000000


def birthday_pools(master: Path) -> list[dict]:
    """Each birthday card's first series, in start order.

    Nine 2024 birthday cards were run again a year later, next to the new
    birthday series, under the same name and art. With every series open at
    once the rerun only duplicates the first one, so it is left out.
    """
    pools = sorted((row for row in load(master, "GachaSeries.yaml")
                    if row["GachaType"] == 1 and "BIRTHDAY" in row["GachaSeriesName"].upper()),
                   key=lambda row: (row["StartTime"], row["Id"]))
    first: dict[int, dict] = {}
    for row in pools:
        first.setdefault(row["PickUpCardSeriesId_1"], row)
    return sorted(first.values(), key=lambda row: (row["StartTime"], row["Id"]))


def _unix(value) -> int:
    return calendar.timegm(value.utctimetuple())


def standard_card_release(master: Path, card_ids: list[int]) -> dict[int, int]:
    """Release time (Unix seconds) of each standard card.

    CardSeries.StartTime is a 2022-12-31 placeholder for every card, so a card
    is dated by its first appearance as a Gacha pickup or a point-exchange
    offer. A card that never appeared there joined with its generation (the
    third id digit: 102..105期) and takes that generation's earliest date.
    """
    first: dict[int, object] = {}
    for pool in load(master, "GachaSeries.yaml"):
        for i in range(1, 7):
            series_id = pool[f"PickUpCardSeriesId_{i}"]
            if series_id:
                first[series_id] = min(first.get(series_id, pool["StartTime"]), pool["StartTime"])
    for row in load(master, "ExchangePointRate.yaml"):
        if row["ExchangeItemType"] == 2:
            series_id = row["ExchangeItemId"] // 10
            first[series_id] = min(first.get(series_id, row["StartTime"]), row["StartTime"])
    dated = {card: first[card // 10] for card in card_ids if card // 10 in first}
    by_generation: dict[str, object] = {}
    for card, start in dated.items():
        generation = str(card)[2]
        by_generation[generation] = min(by_generation.get(generation, start), start)
    return {card: _unix(dated.get(card) or by_generation[str(card)[2]]) for card in card_ids}


def load(master: Path, name: str) -> list[dict]:
    with (master / name).open(encoding="utf-8") as stream:
        return yaml.safe_load(stream) or []


def generate(master: Path) -> str:
    series = {row["Id"]: row for row in load(master, "CardSeries.yaml")}
    pools = {row["Id"]: row for row in load(master, "GachaSeries.yaml")}
    cutoff = min(pools[pool_id]["StartTime"] for pool_id in POOL_IDS)
    coordinates = {row["CardDatasId"] for row in load(master, "CardCoordinates.yaml")}
    movies = {row["Id"] for row in load(master, "CardGetMovieSettings.yaml")}
    acquisition_movies = {row["CardSeriesId"] for row in load(master, "StyleMovies.yaml")
                          if row["MovieType"] == 1}
    by_rarity: dict[int, list[int]] = {3: [], 4: [], 5: []}
    for row in load(master, "CardDatas.yaml"):
        rarity = row["Rarity"]
        if rarity not in by_rarity or row["EvolveTimes"] != 0:
            continue
        card_series = series[row["CardSeriesId"]]
        if card_series["LimitedType"] != 0 or card_series["StartTime"] > cutoff:
            continue
        if "？" in row["Name"] or "?" in row["Name"]:
            continue  # Unreleased placeholders have no usable card art.
        card_id = row["Id"]
        if card_id not in coordinates or (rarity == 5 and
                                           (card_id not in movies or row["CardSeriesId"] not in acquisition_movies)):
            raise ValueError(f"card {card_id} lacks required Gacha animation metadata")
        by_rarity[rarity].append(card_id)
    if [len(by_rarity[r]) for r in (3, 4, 5)] != [46, 42, 73]:
        raise ValueError("unexpected R2606000 standard-card counts")

    # Birthday pools: a series' standard candidates are the standard cards
    # released by its start; the featured card is its BR pickup (whose first
    # CardDatas record has EvolveTimes 1, so its id ends in 1, not 0).
    cards_by_series: dict[int, list[dict]] = {}
    for row in load(master, "CardDatas.yaml"):
        cards_by_series.setdefault(row["CardSeriesId"], []).append(row)
    card_start = standard_card_release(master, [c for r in (3, 4, 5) for c in by_rarity[r]])
    birthdays = []
    for order, pool in enumerate(birthday_pools(master), start=BIRTHDAY_ORDER_BASE):
        pickups = [pool[f"PickUpCardSeriesId_{i}"] for i in range(1, 7) if pool[f"PickUpCardSeriesId_{i}"]]
        if len(pickups) != 1:
            raise ValueError(f"birthday series {pool['Id']} has {len(pickups)} pickups")
        featured = min(cards_by_series[pickups[0]], key=lambda row: row["EvolveTimes"])
        if featured["Id"] not in coordinates or featured["Id"] not in movies:
            raise ValueError(f"birthday card {featured['Id']} lacks Gacha animation metadata")
        birthdays.append({"id": pool["Id"], "exchange_point_id": pool["GachaExchangePointId"], "order_id": order,
                          "start": _unix(pool["StartTime"]), "featured": featured["Id"]})

    digest = hashlib.sha256()
    for name in ("CardDatas.yaml", "CardSeries.yaml", "GachaSeries.yaml",
                 "CardCoordinates.yaml", "CardGetMovieSettings.yaml",
                 "StyleMovies.yaml", "ExchangePointRate.yaml"):
        digest.update((master / name).read_bytes())
    lines = [
        "// Generated by tools/generate_gacha_catalog.py; do not edit manually.",
        f"// R2606000 Master input SHA-256: {digest.hexdigest()}",
        "#pragma once", "#include <array>", "", "namespace LinkuraLocal::HttpMock {",
    ]
    for rarity, label in ((3, "R"), (4, "SR"), (5, "UR")):
        ids = sorted(by_rarity[rarity])
        lines.append(f"inline constexpr std::array<int, {len(ids)}> kMuseumStandard{label}Cards{{{{")
        for offset in range(0, len(ids), 8):
            lines.append("    " + ", ".join(map(str, ids[offset:offset + 8])) + ",")
        lines.append("}};")
        lines.append("")
    lines.append("// CardSeries.StartTime (Unix seconds) of each standard card above.")
    lines.append("struct MuseumCardStart { int cardId; long long start; };")
    starts = sorted(card_start.items())
    lines.append(f"inline constexpr std::array<MuseumCardStart, {len(starts)}> kMuseumStandardCardStart{{{{")
    for offset in range(0, len(starts), 4):
        lines.append("    " + " ".join(f"{{{c}, {t}}}," for c, t in starts[offset:offset + 4]))
    lines.append("}};")
    lines.append("")
    lines.extend([
        "// Birthday series (GachaType 1), after the seven closing series in this order.",
        "struct MuseumBirthdayPool { int id; int exchangePointId; long long start; int featuredCard; };",
        f"inline constexpr std::array<MuseumBirthdayPool, {len(birthdays)}> kMuseumBirthdayPools{{{{",
    ])
    for pool in birthdays:
        lines.append(f"    {{{pool['id']}, {pool['exchange_point_id']}, {pool['start']}, {pool['featured']}}},")
    lines.extend(["}};", ""])
    exchange_pools = set(CLOSING_POOL_IDS) | {pool["id"] for pool in birthdays}
    offers = [row for row in load(master, "ExchangePointRate.yaml")
              if row["GachaSeriesId"] in exchange_pools and row["ExchangeItemType"] == 2]
    cards_by_id = {row["Id"]: row for row in load(master, "CardDatas.yaml")}
    for row in offers:
        card = cards_by_id.get(row["ExchangeItemId"])
        if (not card or card["Id"] not in coordinates or
                (card["Rarity"] == 5 and (card["Id"] not in movies or
                                          card["CardSeriesId"] not in acquisition_movies)) or
                (card["Rarity"] == 9 and card["Id"] not in movies)):
            raise ValueError(f"exchange offer {row['Id']} has no verified card animation")
    lines.extend([
        "struct MuseumExchangeOffer {",
        "    int rateId, pointId, seriesId, cardId, price, limit, quantity, bonus;",
        "};",
        f"inline constexpr std::array<MuseumExchangeOffer, {len(offers)}> kMuseumExchangeOffers{{{{",
    ])
    for row in sorted(offers, key=lambda item: (item["GachaSeriesId"], item["Id"])):
        lines.append("    {" + ", ".join(str(row[key]) for key in (
            "Id", "ExchangePointId", "GachaSeriesId", "ExchangeItemId",
            "ExchangePrice", "LimitedCount", "ExchangeItemQuantity", "BonusItemQuantity")) + "},")
    lines.extend(["}};", "", "} // namespace LinkuraLocal::HttpMock"])
    return "\n".join(lines) + "\n"


def opened_series(master: Path) -> str:
    """The Master rows master-open-gacha patches: EndTime and OrderId."""
    return json.dumps({
        "end_ticks": OPEN_END_TICKS,
        "series": [{"id": pool["Id"], "order_id": order}
                   for order, pool in enumerate(birthday_pools(master), start=BIRTHDAY_ORDER_BASE)],
    }, indent=2) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--master-dir", type=Path, default=DEFAULT_MASTER)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    outputs = {OUTPUT: generate(args.master_dir), OPENED_SERIES_OUTPUT: opened_series(args.master_dir)}
    for path, expected in outputs.items():
        if args.check:
            if path.read_text(encoding="utf-8") != expected:
                raise SystemExit(f"{path.name} is stale; regenerate it")
        else:
            path.write_text(expected, encoding="utf-8")
            print(f"wrote {path}")


if __name__ == "__main__":
    main()
