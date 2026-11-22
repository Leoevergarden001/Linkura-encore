"""Host tests execute the exact SQL embedded by the Android runtime."""
from __future__ import annotations

import json
from contextlib import closing
from pathlib import Path
import sqlite3
import tempfile
import unittest

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from paths import BUILTIN_DIR, DB_DIR, GENERATORS_DIR

BACKEND = DB_DIR
BUILTIN = BUILTIN_DIR


def sql(path: str) -> str:
    return (BACKEND / path).read_text(encoding="utf-8")


def create_schema(db: sqlite3.Connection) -> None:
    for path in sorted((BACKEND / "schema").glob("*.sql")):
        db.executescript(path.read_text(encoding="utf-8"))


def bootstrap(db: sqlite3.Connection) -> None:
    # executescript commits a pending transaction. Include the transaction in
    # the script here; Android wraps the same statements with BEGIN/COMMIT.
    db.executescript("BEGIN IMMEDIATE;\n" + sql("player-state/001_museum_bootstrap.sql") + "\nCOMMIT;")


def seed_fixture(db: sqlite3.Connection) -> None:
    db.execute("INSERT INTO card_detail(d_card_datas_id, response_json, style_level) VALUES('card-1', '{}', 40)")
    db.execute("INSERT INTO item(d_item_datas_id, response_json, item_id, item_num) VALUES('item-1', '{\"name\":\"item\",\"item_num\":99}', 1, 99)")
    for table, id_column, payloads in (
        ("collection_music", "musics_id", [{"musics_id": 1}, {"musics_id": 2}]),
        ("collection_sticker", "stickers_id", [
            {"stickers_id": 1, "is_owned": True, "requirement_progress": 4},
            {"stickers_id": 2, "is_owned": False, "requirement_progress": 0},
        ]),
        ("collection_gallary", "comics_id", [{"comics_id": 1}]),
    ):
        for payload in payloads:
            db.execute(f"INSERT INTO {table}({id_column}, response_json) VALUES(?, ?)",
                       (payload[id_column], json.dumps(payload)))
    db.execute("INSERT INTO music_mastery(music_id, is_mastery) VALUES(1, 1)")
    db.execute("INSERT INTO quest_stage(stage_id) VALUES(1)")
    db.commit()


class MuseumStateTests(unittest.TestCase):
    def test_museum_tutorial_contract_is_complete_and_templates_agree(self) -> None:
        login = json.loads((BUILTIN / "user_login.json").read_text())
        home = json.loads((BUILTIN / "home_get_home.json").read_text())
        template = json.loads((GENERATORS_DIR / "template/home_get_home.json").read_text())
        self.assertFalse(login["is_tutorial"])
        self.assertTrue(all(row["is_complete"] for row in login["tutorials_status_list"]))
        self.assertTrue(home["is_finish_tutorial"])
        self.assertEqual(home["is_finish_tutorial"], template["is_finish_tutorial"])

    def setUp(self) -> None:
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        create_schema(self.db)
        seed_fixture(self.db)
        bootstrap(self.db)

    def tearDown(self) -> None:
        self.db.close()

    def test_home_counts_are_player_state_not_catalog_counts(self) -> None:
        counts = dict(self.db.execute(sql("queries/home_counts.sql")).fetchone())
        self.assertEqual(counts, dict(card_num=1, dream_style_num=0, music_num=2, music_max_num=2,
                                     sticker_num=1, gallary_num=1, standard_live_max_num=1))
        # Sticker collection includes the catalog's unowned entries as well.
        stickers = self.db.execute(sql("queries/collection_sticker.sql")).fetchall()
        self.assertEqual([row[1] for row in stickers], [1, 0])

    def test_restart_does_not_regrant_removed_ownership_or_reset_quantities(self) -> None:
        self.db.execute("UPDATE museum_card_state SET style_level = 20")
        self.db.execute("UPDATE museum_item_inventory SET item_num = 7")
        self.db.execute("DELETE FROM museum_music_ownership WHERE musics_id = 2")
        self.db.execute("DELETE FROM museum_sticker_ownership")
        self.db.execute("DELETE FROM museum_gallary_ownership")
        self.db.commit()
        bootstrap(self.db)
        self.assertEqual(self.db.execute("SELECT style_level FROM museum_card_state").fetchone()[0], 20)
        self.assertEqual(self.db.execute("SELECT item_num FROM museum_item_inventory").fetchone()[0], 7)
        counts = dict(self.db.execute(sql("queries/home_counts.sql")).fetchone())
        self.assertEqual((counts["music_num"], counts["music_max_num"], counts["sticker_num"], counts["gallary_num"]),
                         (1, 2, 0, 0))
        self.assertEqual(len(self.db.execute(sql("queries/collection_music.sql")).fetchall()), 1)
        self.assertEqual(len(self.db.execute(sql("queries/collection_gallary.sql")).fetchall()), 0)
        self.assertEqual(self.db.execute(sql("queries/collection_sticker.sql")).fetchone()[2], 0)

    def test_item_detail_and_list_share_quantity_without_mutating_catalog(self) -> None:
        self.db.execute("UPDATE museum_item_inventory SET item_num = 7")
        self.assertEqual(self.db.execute(sql("queries/item_detail.sql"), ("item-1",)).fetchone()[1], 7)
        self.assertEqual(self.db.execute(sql("queries/item_list.sql")).fetchone()[5], 7)
        self.assertEqual(self.db.execute("SELECT item_num FROM item").fetchone()[0], 99)

    def test_music_history_reads_score_and_commits_last_play_time_atomically(self) -> None:
        self.db.execute("INSERT INTO rhythm_music_score(music_id, clear_count) VALUES(1, 3)")
        self.db.execute(sql("queries/rhythm_collection_finish.sql"), (1,))
        music = self.db.execute(sql("queries/collection_music.sql")).fetchone()
        self.assertEqual((music[1], music[2]), (1, 3))
        self.assertNotEqual(music[3], "0001-01-01T00:00:00Z")
        self.db.rollback()
        music = self.db.execute(sql("queries/collection_music.sql")).fetchone()
        self.assertEqual((music[2], music[3]), (0, "0001-01-01T00:00:00Z"))

    def test_invalid_inventory_and_unknown_ids_are_rejected(self) -> None:
        for statement in (
            "UPDATE museum_item_inventory SET item_num = -1",
            "INSERT INTO museum_item_inventory VALUES('unknown', 1)",
            "INSERT INTO museum_music_ownership(musics_id) VALUES(999)",
        ):
            with self.assertRaises(sqlite3.IntegrityError):
                self.db.execute(statement)

    def test_existing_card_state_survives_first_migration(self) -> None:
        self.db.execute("DELETE FROM museum_bootstrap_state WHERE domain = 'CARD'")
        self.db.execute("UPDATE museum_card_state SET style_level = 10")
        self.db.commit()
        bootstrap(self.db)
        self.assertEqual(self.db.execute("SELECT style_level FROM museum_card_state").fetchone()[0], 10)

    def test_reset_respects_foreign_keys_and_allows_explicit_rebootstrap(self) -> None:
        self.db.executescript(sql("queries/reset_museum_state.sql"))
        self.assertEqual(self.db.execute("SELECT COUNT(*) FROM museum_bootstrap_state").fetchone()[0], 0)
        # Catalog reset is explicit, not part of normal startup.
        self.db.execute("DELETE FROM card_detail")
        self.db.execute("DELETE FROM item")
        self.db.commit()
        self.assertEqual(self.db.execute("PRAGMA foreign_key_check").fetchall(), [])

    def test_file_reopen_preserves_inventory_and_bootstrap_marker(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "player.sqlite3"
            with closing(sqlite3.connect(path)) as db:
                create_schema(db)
                seed_fixture(db)
                bootstrap(db)
                db.execute("UPDATE museum_item_inventory SET item_num = 3")
                db.commit()
            with closing(sqlite3.connect(path)) as db:
                create_schema(db)
                bootstrap(db)
                self.assertEqual(db.execute("SELECT item_num FROM museum_item_inventory").fetchone()[0], 3)

    def test_real_builtin_catalog_and_seed_have_matching_counts(self) -> None:
        with closing(sqlite3.connect(":memory:")) as db:
            create_schema(db)
            db.executescript(sql("seed/001_init.sql"))
            expected = {}
            for kind, id_column, list_key in (
                ("music", "musics_id", "music_info_list"),
                ("sticker", "stickers_id", "sticker_info_list"),
                ("gallary", "comics_id", "gallary_info_list"),
            ):
                payloads = json.loads((BUILTIN / f"collection_get_{kind}_list.json").read_text(encoding="utf-8"))[list_key]
                db.executemany(f"INSERT INTO collection_{kind}({id_column}, response_json) VALUES(?, ?)",
                               [(payload[id_column], json.dumps(payload)) for payload in payloads])
                expected[kind] = len(payloads) if kind != "sticker" else sum(p["is_owned"] for p in payloads)
            db.commit()
            bootstrap(db)
            db.row_factory = sqlite3.Row
            counts = db.execute(sql("queries/home_counts.sql")).fetchone()
            self.assertEqual((counts["music_num"], counts["music_max_num"], counts["sticker_num"], counts["gallary_num"]),
                             (expected["music"], expected["music"], expected["sticker"], expected["gallary"]))
            self.assertEqual(counts["card_num"], db.execute("SELECT COUNT(*) FROM card_detail").fetchone()[0])
            self.assertEqual(db.execute("PRAGMA foreign_key_check").fetchall(), [])


if __name__ == "__main__":
    unittest.main()
