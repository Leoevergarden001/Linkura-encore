"""Audit Gacha card metadata against the saved Android resource archive.

The client cache uses opaque D/ file names. Without its catalog, a present D/
entry cannot be attributed to a card. This report deliberately never calls a
card's art, movie, or voice verified on the basis of Master rows alone.
"""

from __future__ import annotations

import argparse
import json
import tarfile
from pathlib import Path

from generate_gacha_catalog import DEFAULT_MASTER, load

DEFAULT_ARCHIVE = (DEFAULT_MASTER.parent / "clients/android/author-beta5-20261025"
                   / "backups/preservation-snapshot-20261025T014511Z/game-external-files.tar")
KNOWN_ANIMATION_FAILURES = {10215210}
FEATURED = {
    1055003: (19415010, 19425010, 19435010, 19315020, 19525010),
    1055002: (19215020, 19235010, 19425020, 19515010),
    1055001: (19215010, 19225010, 19235010, 19315010, 19335010),
    1050511: (10305010,),
    1050509: (10315390, 10325370, 10335360),
    1050506: (10215170, 10225170, 10225180, 10235140, 10115010),
    1050505: (10215270, 10225270, 10235240, 10205010),
}


def audit(master: Path, archive: Path) -> dict:
    card_data = {row["Id"]: row for row in load(master, "CardDatas.yaml")}
    coordinates = {row["CardDatasId"] for row in load(master, "CardCoordinates.yaml")}
    get_movie = {row["Id"] for row in load(master, "CardGetMovieSettings.yaml")}
    style_movies = {row["CardSeriesId"] for row in load(master, "StyleMovies.yaml")
                    if row["MovieType"] == 1}
    series = {row["Id"]: row for row in load(master, "CardSeries.yaml")}
    pools = {row["Id"]: row for row in load(master, "GachaSeries.yaml")}
    cutoff = min(pools[pool_id]["StartTime"] for pool_id in (1055001, 1055002, 1055003))
    standard = {row["Id"] for row in card_data.values()
                if row["Rarity"] in (3, 4, 5) and row["EvolveTimes"] == 0
                and series[row["CardSeriesId"]]["LimitedType"] == 0
                and series[row["CardSeriesId"]]["StartTime"] <= cutoff
                and "?" not in row["Name"] and "？" not in row["Name"]}
    featured = {card for ids in FEATURED.values() for card in ids}
    cards = []
    for card_id in sorted(standard | featured):
        row = card_data.get(card_id)
        metadata = {
            "card_data": row is not None,
            "coordinates": card_id in coordinates,
            "get_movie_setting": card_id in get_movie if row and row["Rarity"] == 5 else None,
            "style_acquisition_movie": row["CardSeriesId"] in style_movies
            if row and row["Rarity"] == 5 else None,
        }
        cards.append({
            "card_id": card_id,
            "rarity": row["Rarity"] if row else None,
            "standard": card_id in standard,
            "featured_series": [pool for pool, ids in FEATURED.items() if card_id in ids],
            "master": metadata,
            "animation_status": "OBSERVED_FAILURE" if card_id in KNOWN_ANIMATION_FAILURES
                                else "ASSET_UNVERIFIED",
        })
    count = 0
    bytes_total = 0
    with tarfile.open(archive, "r") as bundle:
        for member in bundle:
            if member.isfile() and member.name.startswith("./D/"):
                count += 1
                bytes_total += member.size
    return {
        "master": str(master), "archive": str(archive),
        "archive_d_files": count, "archive_d_bytes": bytes_total,
        "standard_cards": len(standard), "featured_cards": len(featured),
        "cards": cards,
        "limitation": "D/ files are opaque; no per-card asset verification without a catalog mapping",
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--master-dir", type=Path, default=DEFAULT_MASTER)
    parser.add_argument("--archive", type=Path, default=DEFAULT_ARCHIVE)
    args = parser.parse_args()
    print(json.dumps(audit(args.master_dir, args.archive), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
