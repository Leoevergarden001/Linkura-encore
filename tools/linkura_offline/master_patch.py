"""Open Gacha series that ended before the service closed.

The client lists a Gacha series only while the server clock lies between its
Master StartTime and EndTime (GachaSceneController.SetGachaInfoList), so a
series that ended before the shutdown never appears, whatever the server
returns. ``open_gacha_series`` copies a cache's master database and sets the
listed series' EndTime (and their point-exchange offers') to the date of the
series still open at the end of service, and their OrderId so they follow the
seven closing series. The list comes from server/data/museum_gacha_opened_series.json
(written by server/generators/generate_gacha_catalog.py), so the server and the
Master agree. The output keeps the database's name and storage key; replace the
device's M/<name> with it.
"""
from __future__ import annotations

import json
import shutil
from dataclasses import dataclass
from pathlib import Path

from .cache_rekey import (MASTER_PREFS_KEY, RekeyError, _sqlcipher, decode_seed, read_storage_key,
                          sqlcipher_key, storage_name)

REPO = Path(__file__).resolve().parents[2]
DEFAULT_SERIES = REPO / "server" / "data" / "museum_gacha_opened_series.json"


@dataclass(frozen=True)
class OpenedGacha:
    database: Path
    series: int
    exchange_offers: int


def load_series(path: Path = DEFAULT_SERIES) -> tuple[int, list[tuple[int, int]]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    return int(data["end_ticks"]), [(int(s["id"]), int(s["order_id"])) for s in data["series"]]


def open_gacha_series(m_dir: Path, storage_key: Path, output: Path, *, sqlcipher: str = "sqlcipher",
                      series_file: Path = DEFAULT_SERIES) -> OpenedGacha:
    if shutil.which(sqlcipher) is None:
        raise RekeyError(f"{sqlcipher!r} not found; install SQLCipher 4 (e.g. brew install sqlcipher)")
    value = read_storage_key(storage_key)["entries"].get(MASTER_PREFS_KEY)
    if not value:
        raise RekeyError(f"{storage_key} has no master database entry")
    seed = decode_seed(value)
    source = m_dir / storage_name(seed)
    if not source.is_file():
        raise RekeyError(f"no master database for this storage key in {m_dir}")
    target = output / "M" / source.name
    if target.exists():
        raise RekeyError(f"{target} already exists")
    end_ticks, series = load_series(series_file)
    if not series:
        raise RekeyError(f"{series_file} lists no series")

    target.parent.mkdir(parents=True, exist_ok=True)
    work = target.with_name(target.name + ".partial")
    shutil.copy2(source, work)
    try:
        ids = ",".join(str(series_id) for series_id, _ in series)
        updates = "".join(f"UPDATE GachaSeries SET EndTime = {end_ticks}, OrderId = {order} WHERE Id = {series_id};\n"
                          for series_id, order in series)
        out = _sqlcipher(sqlcipher, work,
                         f"PRAGMA key = \"x'{sqlcipher_key(seed)}'\";\n"
                         f"SELECT 'found=' || count(*) FROM GachaSeries WHERE Id IN ({ids});\n"
                         "BEGIN;\n" + updates +
                         f"UPDATE ExchangePointRate SET EndTime = {end_ticks} "
                         f"WHERE GachaSeriesId IN ({ids}) AND EndTime < {end_ticks};\n"
                         "SELECT 'offers=' || changes();\n"
                         "COMMIT;\n"
                         f"SELECT 'opened=' || count(*) FROM GachaSeries WHERE Id IN ({ids}) AND EndTime = {end_ticks};\n")
        values = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
        if int(values.get("found", -1)) != len(series) or int(values.get("opened", -1)) != len(series):
            raise RekeyError(f"master database lacks some of the {len(series)} series ({values})")
        journal = work.with_name(work.name + "-journal")
        if journal.exists():
            journal.unlink()
        work.rename(target)
        target.chmod(0o644)
    except BaseException:
        work.unlink(missing_ok=True)
        raise
    return OpenedGacha(database=target, series=len(series), exchange_offers=int(values["offers"]))
