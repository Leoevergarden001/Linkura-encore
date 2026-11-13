import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.linkura_offline.cache_rekey import MASTER_PREFS_KEY, RekeyError, encode_seed, sqlcipher_key, storage_name
from tools.linkura_offline.master_patch import DEFAULT_SERIES, load_series, open_gacha_series

HAVE_SQLCIPHER = shutil.which("sqlcipher") is not None


def _sql(db: Path, seed: bytes, statements: str) -> str:
    script = f"PRAGMA key = \"x'{sqlcipher_key(seed)}'\";\n{statements}\n"
    return subprocess.run(["sqlcipher", "-batch", "-bail", str(db)], input=script, text=True,
                          capture_output=True, check=True).stdout


class SeriesFileTest(unittest.TestCase):
    def test_checked_in_list_is_each_birthday_cards_first_series(self):
        end, series = load_series(DEFAULT_SERIES)
        self.assertEqual(end, 642183227990000000)
        self.assertEqual(len(series), 17)  # one series per birthday card: its first run
        self.assertEqual([order for _, order in series], list(range(101, 118)))
        self.assertNotIn(1050112, [series_id for series_id, _ in series])  # Kaho's 2024 card rerun in 2025


@unittest.skipUnless(HAVE_SQLCIPHER, "sqlcipher not installed")
class OpenGachaTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.seed = os.urandom(256)
        self.m_dir = self.tmp / "M"
        self.m_dir.mkdir()
        self.db = self.m_dir / storage_name(self.seed)
        _sql(self.db, self.seed,
             "CREATE TABLE GachaSeries(Id INTEGER PRIMARY KEY, OrderId INTEGER, StartTime INTEGER, EndTime INTEGER);"
             "CREATE TABLE ExchangePointRate(Id INTEGER PRIMARY KEY, GachaSeriesId INTEGER, EndTime INTEGER);"
             "INSERT INTO GachaSeries VALUES (1, 1, 0, 9), (2, 50, 0, 5), (3, 60, 0, 5);"
             "INSERT INTO ExchangePointRate VALUES (10, 2, 5), (11, 3, 5), (12, 1, 9);"
             "CREATE TABLE SelectTicketSeries(Id INTEGER PRIMARY KEY, EndTime INTEGER);"
             "INSERT INTO SelectTicketSeries VALUES (20, 4), (21, 9), (22, 12);")
        self.key = self.tmp / "storage-key.json"
        self.key.write_text(json.dumps({"prefs": "ios", "entries": {MASTER_PREFS_KEY: encode_seed(self.seed)}}))
        self.series = self.tmp / "series.json"
        self.series.write_text(json.dumps({"end_ticks": 9, "series": [{"id": 2, "order_id": 101},
                                                                       {"id": 3, "order_id": 102}]}))

    def test_opens_listed_series_and_offers_in_a_copy(self):
        result = open_gacha_series(self.m_dir, self.key, self.tmp / "out", series_file=self.series)
        self.assertEqual((result.series, result.exchange_offers, result.select_tickets), (2, 2, 1))
        self.assertEqual(result.database.name, self.db.name)
        rows = _sql(result.database, self.seed, "SELECT Id, OrderId, EndTime FROM GachaSeries ORDER BY Id;")
        self.assertEqual(rows.split()[1:], ["1|1|9", "2|101|9", "3|102|9"])
        self.assertEqual(_sql(result.database, self.seed, "SELECT group_concat(EndTime) FROM ExchangePointRate;").split()[-1],
                         "9,9,9")
        self.assertEqual(_sql(result.database, self.seed, "SELECT group_concat(EndTime) FROM SelectTicketSeries;").split()[-1],
                         "9,9,12")
        self.assertEqual(_sql(self.db, self.seed, "SELECT EndTime FROM GachaSeries WHERE Id = 2;").split()[-1], "5")

    def test_refuses_a_master_without_the_series(self):
        self.series.write_text(json.dumps({"end_ticks": 9, "series": [{"id": 4, "order_id": 101}]}))
        with self.assertRaises(RekeyError):
            open_gacha_series(self.m_dir, self.key, self.tmp / "out", series_file=self.series)
        self.assertFalse(list((self.tmp / "out" / "M").glob("*")))

    def test_does_not_overwrite(self):
        open_gacha_series(self.m_dir, self.key, self.tmp / "out", series_file=self.series)
        with self.assertRaises(RekeyError):
            open_gacha_series(self.m_dir, self.key, self.tmp / "out", series_file=self.series)
