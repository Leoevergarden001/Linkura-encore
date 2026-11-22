#!/usr/bin/env python3
"""Smoke test for the offline API server (host build).

    server/build-host.sh && python3 server/tests/smoke_test.py

Starts build/host/linkura-offline-server on a free port with a temporary
data directory and checks the HTTP framing the game's .NET HttpClient uses
(keep-alive, Expect: 100-continue, chunked request bodies) plus a few routes.
"""
from __future__ import annotations

import base64
import http.client
import json
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BINARY = Path(__file__).resolve().parents[1] / "build/host/linkura-offline-server"


def yaml_rows(name: str) -> list[dict]:
    """Rows of a link-like-diff Master YAML (flat "- Key: value" records, ints parsed)."""
    rows, row = [], None
    for line in (Path(__file__).resolve().parents[2] / "third_party" / "link-like-diff" / name).read_text(
            encoding="utf-8").splitlines():
        if line.startswith("- "):
            row = {}
            rows.append(row)
            line = line[2:]
        key, sep, value = line.strip().partition(": ")
        if row is not None and sep:
            value = value.strip().strip('"')
            row[key] = int(value) if value.lstrip("-").isdigit() else value
    return rows


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def start(data: Path, port: int, *extra: str) -> subprocess.Popen:
    proc = subprocess.Popen([str(BINARY), "--data", str(data), "--port", str(port), *extra],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    line = proc.stdout.readline()
    if not line.startswith("READY"):
        raise SystemExit(f"server did not start: {line!r} {proc.stderr.read()}")
    return proc


def stop(proc: subprocess.Popen) -> str:
    proc.stdin.close()
    proc.wait(timeout=10)
    return proc.stderr.read()


def post(conn: http.client.HTTPConnection, path: str, body: dict, headers: dict | None = None):
    conn.request("POST", path, body=json.dumps(body), headers={"Content-Type": "application/json", **(headers or {})})
    response = conn.getresponse()
    return response.status, dict(response.getheaders()), response.read()


def main() -> int:
    if not BINARY.exists():
        print(f"missing {BINARY}; run server/build-host.sh first")
        return 2
    failures = []

    def check(name: str, condition: bool, detail: str = "") -> None:
        print(("ok   " if condition else "FAIL ") + name + (f"  ({detail})" if detail and not condition else ""))
        if not condition:
            failures.append(name)

    with tempfile.TemporaryDirectory() as tmp:
        data = Path(tmp)
        port = free_port()
        proc = start(data, port)
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
            status, headers, body = post(conn, "/v1/user/login",
                                         {"player_id": "", "device_specific_id": "", "version": 1})
            login = json.loads(body)
            check("login 200", status == 200, str(status))
            check("login session_token", bool(login.get("session_token")))
            check("no x-res-version by default", "x-res-version" not in {k.lower() for k in headers})
            status, _, body = post(conn, "/v1/home/get_home", {})  # same connection (keep-alive)
            home = json.loads(body)
            check("home on kept-alive connection", status == 200 and "profile_info" in home)
            status, _, body = post(conn, "/v1/gacha/get_series_list", {})
            check("gacha series list", status == 200 and len(body) > 1000, f"{status} {len(body)}")
            status, _, _ = post(conn, "/v1/definitely/missing", {})
            check("unknown route 404", status == 404, str(status))
            conn.close()

            # Expect: 100-continue and chunked body over a raw socket.
            with socket.create_connection(("127.0.0.1", port), timeout=30) as s:
                payload = b'{"player_id":"","device_specific_id":"","version":1}'
                s.sendall(b"POST /v1/user/login HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\n"
                          b"Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n")
                first = s.recv(64)
                check("100 Continue sent", first.startswith(b"HTTP/1.1 100 Continue"), first[:40].decode(errors="replace"))
                s.sendall(b"%x\r\n%s\r\n0\r\n\r\n" % (len(payload), payload))
                reply = b""
                while chunk := s.recv(65536):
                    reply += chunk
                check("chunked body handled", b"200 OK" in reply and b"session_token" in reply)
        finally:
            log = stop(proc)
        check("database created", (data / "HasuKikaisann.sqlite3").exists())

        # One card per series (card id / 10): the Museum owns the fully developed record, not also
        # the base records EnsureMuseumGachaCardsLocked adds for draws (a Lv1 copy in the card list).
        def series_counts(db):
            return db.execute("SELECT COUNT(*), COUNT(DISTINCT c.card_datas_id / 10) FROM museum_card_ownership o "
                              "JOIN card_detail c USING(d_card_datas_id)").fetchone()
        with sqlite3.connect(data / "HasuKikaisann.sqlite3") as db:
            owned, series_owned = series_counts(db)
            check("bootstrap owns one record per card series", owned == series_owned, f"{owned}/{series_owned}")

        # An upgrade re-seeds the catalogs when a new catalog table is empty. With foreign keys on,
        # the item rows' INSERT OR REPLACE cascaded into museum_item_inventory and emptied it. Here:
        # a save that lost some inventory rows that way, a player-changed count, and a forced re-seed.
        with sqlite3.connect(data / "HasuKikaisann.sqlite3") as db:
            def inventory(item_id):
                row = db.execute("SELECT m.item_num FROM item i JOIN museum_item_inventory m USING(d_item_datas_id) "
                                 "WHERE i.item_id=?", (item_id,)).fetchone()
                return row[0] if row else None
            capped_lost = db.execute("SELECT i.item_id FROM item i JOIN museum_item_inventory m USING(d_item_datas_id) "
                                     "WHERE i.item_num = 9999 LIMIT 1").fetchone()[0]
            lost_value = inventory(capped_lost)
            db.execute("UPDATE museum_item_inventory SET item_num=123 WHERE d_item_datas_id="
                       "(SELECT d_item_datas_id FROM item WHERE item_id=3011001)")
            db.execute("DELETE FROM museum_item_inventory WHERE d_item_datas_id IN (SELECT d_item_datas_id FROM item "
                       "WHERE item_id IN (?, 1002001))", (capped_lost,))
            db.execute("DELETE FROM museum_bootstrap_state WHERE domain='INVENTORY_CASCADE_REPAIR'")
            db.execute("DELETE FROM grade_quest_release")  # the next start re-seeds every catalog
            # A save from before 007: a base record owned next to its developed record, used in a deck.
            dup, keep = db.execute(
                "SELECT c.d_card_datas_id, (SELECT h.d_card_datas_id FROM card_detail h WHERE h.card_datas_id / 10 = "
                "c.card_datas_id / 10 ORDER BY h.card_datas_id DESC LIMIT 1) FROM card_detail c WHERE EXISTS("
                "SELECT 1 FROM card_detail h WHERE h.card_datas_id / 10 = c.card_datas_id / 10 AND "
                "h.card_datas_id > c.card_datas_id) LIMIT 1").fetchone()
            db.execute("INSERT INTO museum_card_ownership VALUES (?, '2026-11-10T00:00:00Z', 'MUSEUM')", (dup,))
            db.execute("INSERT INTO museum_card_state(d_card_datas_id, style_level, limit_break_times, is_evolve_max) "
                       "VALUES (?, 1, 0, 0)", (dup,))
            db.execute("INSERT OR REPLACE INTO rhythm_game_deck VALUES ('dedupe-test', 'dedupe', 99, ?)",
                       (json.dumps([{"d_card_datas_id": dup, "rhythm_game_deck_cards_id": "x", "slot_no": 1}]),))
            db.execute("DELETE FROM museum_bootstrap_state WHERE domain='CARD_SERIES_DEDUPE'")
            wallet_free = db.execute("SELECT free_jewels FROM museum_gacha_state WHERE series_id=1055003").fetchone()[0]
            items_total = db.execute("SELECT COUNT(*) FROM item").fetchone()[0]

        # Restart on the same data directory: state persists, no re-bootstrap errors.
        proc = start(data, port)
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
            status, _, _ = post(conn, "/v1/home/get_home", {})
            check("home after restart", status == 200)
            with sqlite3.connect(data / "HasuKikaisann.sqlite3") as db:
                owned, series_owned = series_counts(db)
                deck = json.loads(db.execute("SELECT deck_card_list_json FROM rhythm_game_deck "
                                             "WHERE rhythm_game_deck_id='dedupe-test'").fetchone()[0])
                check("duplicate card records are merged once", owned == series_owned and
                      deck[0]["d_card_datas_id"] == keep and db.execute(
                          "SELECT COUNT(*) FROM museum_card_ownership WHERE d_card_datas_id=?", (dup,)).fetchone()[0] == 0,
                      f"{owned}/{series_owned} deck={deck[0]['d_card_datas_id']} keep={keep}")
                db.execute("DELETE FROM rhythm_game_deck WHERE rhythm_game_deck_id='dedupe-test'")
            with sqlite3.connect(data / "HasuKikaisann.sqlite3") as db:
                rows = db.execute("SELECT COUNT(*) FROM museum_item_inventory").fetchone()[0]
                check("re-seed keeps the inventory and repairs lost rows",
                      rows == items_total and inventory(3011001) == 123 and inventory(capped_lost) == lost_value == 999
                      and inventory(1002001) == wallet_free,
                      f"rows {rows}/{items_total}, 3011001={inventory(3011001)}, {capped_lost}={inventory(capped_lost)}"
                      f"/{lost_value}, 1002001={inventory(1002001)}/{wallet_free}")

            # The Items screen froze on an empty item_category_list: all four categories always come back.
            status, _, body = post(conn, "/v1/user/items/get_list", {})
            check("item list has every category", status == 200 and
                  [c["item_category"] for c in json.loads(body)["item_category_list"]] == [0, 1, 2, 3])
            # Exchange buttons: the client enables one when GetMaxCount > 0, i.e. enough material and
            # (Items.LimitNum - held) >= ProductItemNum. Museum holdings at a 9999 / 999999 cap are
            # lowered once (002, 004), so every standard exchange can be used.
            item_limits = {r["Id"]: r["LimitNum"] for r in yaml_rows("Items.yaml")}
            offers = {r["id"]: r for r in json.loads((Path(__file__).resolve().parents[1] / "data" / "builtin" /
                                                      "item_exchange_catalog.json").read_text(encoding="utf-8"))}
            status, _, body = post(conn, "/v1/item_exchange/get_list_new", {"category_list_id": 106001})
            blocked = []
            for entry in json.loads(body)["item_exchange_list"]:
                offer = offers[entry["item_exchanges_id"]]
                count = min(entry["user_material_item_num"] // offer["material_item_num"],
                            (item_limits[offer["product_item_id"]] - entry["user_product_item_num"]) //
                            offer["product_item_num"])
                if count < 1:
                    blocked.append(entry["item_exchanges_id"])
            check("every standard item exchange is usable", status == 200 and not blocked, str(blocked[:10]))

            # SHOW total mission: nothing until all three class missions reach D2.
            db_path = data / "HasuKikaisann.sqlite3"
            status, _, body = post(conn, "/v1/rhythm_game/home", {})
            check("rhythm home total order 0", status == 200 and json.loads(body)["received_total_mission_order"] == 0)
            status, _, body = post(conn, "/v1/rhythm_game/receive_total_mission", {})
            check("receive total mission, not reached", status == 200 and json.loads(body) ==
                  {"received_total_mission_ids": [], "received_total_mission_order": 0}, body[:200].decode())

            def gp() -> int:
                with sqlite3.connect(db_path) as db:
                    row = db.execute("SELECT COALESCE(SUM(i.item_num),0) FROM item JOIN museum_item_inventory i "
                                     "USING(d_item_datas_id) WHERE item.item_id=3011001").fetchone()
                return row[0]
            before = gp()
            with sqlite3.connect(db_path) as db:
                db.execute("INSERT OR REPLACE INTO rhythm_music_score(music_id,clear_count,total_score_accumulated,"
                           "high_score) VALUES(1,3,1000000,1000000)")
            status, _, body = post(conn, "/v1/rhythm_game/receive_total_mission", {})
            check("receive total mission D2", status == 200 and json.loads(body) ==
                  {"received_total_mission_ids": [10001], "received_total_mission_order": 1}, body[:200].decode())
            check("total mission reward granted", gp() == before + 100, f"{before} -> {gp()}")
            status, _, body = post(conn, "/v1/rhythm_game/receive_total_mission", {})
            check("receive total mission is idempotent", json.loads(body)["received_total_mission_ids"] == [])
            status, _, body = post(conn, "/v1/rhythm_game/home", {})
            check("rhythm home total order 1", json.loads(body)["received_total_mission_order"] == 1)
            # Total mission 4 pays 500 SIsCa: it must reach the gacha wallet the game spends and shows.
            def free_jewels() -> int:
                with sqlite3.connect(db_path) as db:
                    return db.execute("SELECT free_jewels FROM museum_gacha_state WHERE series_id=1055003").fetchone()[0]
            jewels_before = free_jewels()
            with sqlite3.connect(db_path) as db:
                db.execute("INSERT OR REPLACE INTO rhythm_music_score(music_id,clear_count,total_score_accumulated,"
                           "high_score) VALUES(1,40,50000000,30000000)")
            status, headers, body = post(conn, "/v1/rhythm_game/receive_total_mission", {})
            check("SHOW total mission SIsCa reaches the wallet",
                  json.loads(body)["received_total_mission_order"] == 4 and free_jewels() == jewels_before + 500 and
                  f"jewel_free: {jewels_before + 500}" in "\n".join(f"{k}: {v}" for k, v in headers.items()),
                  f"{body[:120].decode()} {jewels_before} -> {free_jewels()}")

            # Daily login bonus: LoginBonuses 101001, 7 stamps of free SIsCa (the Master's amounts), then it loops.
            master_days = sorted((r["DayCount"], r["RewardId"], r["RewardNum"]) for r in yaml_rows("LoginBonusRewardDatas.yaml")
                                 if r["LoginBonusRewardSeriesId"] == 101001)

            def home_login_due() -> bool:
                _, _, body = post(conn, "/v1/home/get_home", {})
                return json.loads(body)["is_login_bonus"]

            def login_bonus() -> list:
                _, _, body = post(conn, "/v1/home/get_login_bonus", {})
                return json.loads(body)["login_bonus_list"][0]["datas"]
            jewels_before = free_jewels()
            due = home_login_due()
            days = login_bonus()
            check("login bonus shows the Master's 7 days",
                  [(d["count"], d["rewards"][0]["reward_id"], d["rewards"][0]["amount"]) for d in days] == master_days,
                  str(master_days))
            # The scene stalls on iOS for now: home never opens it, the route still stamps.
            check("home does not open the login bonus scene; stamp 1 pays 50 SIsCa",
                  not due and [d["is_get"] for d in days] == [True] + [False] * 6 and days[0]["is_now_get"] and
                  free_jewels() == jewels_before + 50, json.dumps(days[:2]))
            days = login_bonus()
            check("login bonus stamps once a day", not home_login_due() and not any(d["is_now_get"] for d in days)
                  and free_jewels() == jewels_before + 50)
            for expected in (2, 3, 4, 5, 6, 7, 1):
                with sqlite3.connect(db_path) as db:
                    db.execute("UPDATE offline_login_bonus SET last_day = last_day - 1")
                days = login_bonus()
            check("login bonus loops after day 7", [d["is_now_get"] for d in days] == [True] + [False] * 6 and
                  free_jewels() == jewels_before + sum(n for _, _, n in master_days) + 50,
                  f"{free_jewels() - jewels_before}")

            # Archive / circle screens that used to 404.
            status, _, body = post(conn, "/v1/archive/get_ranking_list", {"member_id": 1021, "target_period": 1})
            ranking = json.loads(body)
            check("archive ranking", status == 200 and ranking["my_rank_status"] == 3 and
                  len(ranking["cheer_rewards"]) > 0 and ranking["ranking_start_time"].startswith("2026-04-02"), body[:200].decode())
            status, _, body = post(conn, "/v1/archive/get_with_station_list", {"limit": 20, "offset": 0})
            stations = json.loads(body)
            names = [w["name"] for w in stations.get("with_station_list", [])]
            check("with station list has the five episodes, newest first",
                  status == 200 and len(names) == 5 and names[0] == "クイズ！これどんな魔法！？" and
                  len(stations["filterable_characters"]) > 0, body[:200].decode())
            _, _, body = post(conn, "/v1/archive/get_with_station_list", {"limit": 2, "offset": 4})
            check("with station list pages", len(json.loads(body)["with_station_list"]) == 1)
            status, _, body = post(conn, "/v1/archive/get_with_station_data",
                                   {"withstation_id": "bc8d535f-968b-49e9-b4f1-18979c0666d9"})
            station = json.loads(body) if status == 200 else {}
            check("with station detail has its HLS video",
                  station.get("video_url", "").endswith("/archive/hls/20260416/index.m3u8") and
                  len(station.get("chapters", [])) == 2, body[:200].decode())
            status, _, body = post(conn, "/v1/archive/get_home", {})
            recent = json.loads(body).get("with_station_list", []) if status == 200 else []
            check("archive home Recent is the latest With×STATION",
                  [r.get("live_id") for r in recent] == ["bc8d535f-968b-49e9-b4f1-18979c0666d9"], body[:200].decode())
            status, _, body = post(conn, "/v1/archive/get_fes_timeline_data", {"archives_id": "x", "play_time_second": 0})
            check("fes timeline", status == 200 and json.loads(body)["timelines"] == [])
            status, _, body = post(conn, "/v1/profile/get_info", {})
            profile = json.loads(body)["profile_info"]
            check("profile has no circle (circle is locked offline)",
                  profile["guild_name"] == "" and profile["search_guild_key"] == "")
            status, _, body = post(conn, "/v1/profile/set_name", {"name": "テスト"})
            check("set name", status == 200 and json.loads(body) == {"result": True}, body[:200].decode())
            post(conn, "/v1/profile/set_comment", {"comment": "hello"})
            status, _, body = post(conn, "/v1/profile/get_info", {})
            renamed = json.loads(body)["profile_info"]
            check("name/comment persisted", renamed["player_name"] == "テスト" and renamed["comment"] == "hello")
            status, _, body = post(conn, "/v1/home/get_home", {})
            check("home shows new name", json.loads(body)["profile_info"]["player_name"] == "テスト")
            status, headers, body = post(conn, "/v1/home/get_home", {})
            check("name header follows set_name", headers.get("name") == base64.b64encode("テスト".encode()).decode(),
                  str(headers.get("name")))
            status, _, body = post(conn, "/v1/home/get_custom_setting", {})
            custom = json.loads(body)["profile_info"]
            check("custom setting shows new name, no circle", status == 200 and custom["player_name"] == "テスト"
                  and custom["guild_name"] == "", body[:200].decode())
            status, _, body = post(conn, "/v1/user/card/get_list", {})
            owned_cards = json.loads(body)["user_card_data_list"]
            first_card = owned_cards[0]["d_card_datas_id"]
            placeholders = [c["card_datas_id"] for c in owned_cards if c["card_datas_id"] // 10 in (1010400, 1010500)]
            check("no card-less ？？？ cards owned", not placeholders and len(owned_cards) == 549,
                  f"{len(owned_cards)} {placeholders}")
            status, _, body = post(conn, "/v1/follow/set_friend_card",
                                   {"school_idol_stage_card_id": "", "rhythm_game_card_id": ""})
            check("set empty friend cards", status == 200 and json.loads(body) == {"result": True}, body[:200].decode())
            post(conn, "/v1/follow/set_friend_card", {"school_idol_stage_card_id": first_card, "rhythm_game_card_id": ""})
            status, _, body = post(conn, "/v1/profile/get_info", {})
            friend = json.loads(body)["profile_info"]["friend_card_info"]
            check("friend card shown on profile", len(friend) == 1 and friend[0]["card_type"] == 1 and
                  friend[0]["user_card_info"]["d_card_datas_id"] == first_card, json.dumps(friend)[:200])
            # 1日1回限定 paid draw: available once per day, paid SIsCa only, 50.
            status, _, body = post(conn, "/v1/gacha/get_series_list", {})
            first = json.loads(body)["gacha_series_list"][0]
            check("daily paid draw offered", first["is_paid_enable"] and first["remaining_limited_paid_gacha_count"] == 1)
            status, _, body = post(conn, "/v1/gacha/set_purchase",
                                   {"gacha_series_id": first["gacha_series_id"], "gacha_set_purchase_type": 3, "campaign_id": 0})
            check("daily paid draw", status == 200, body[:200].decode())
            status, _, body = post(conn, "/v1/gacha/set_purchase",
                                   {"gacha_series_id": first["gacha_series_id"], "gacha_set_purchase_type": 3, "campaign_id": 0})
            check("daily paid draw only once", status == 400, str(status))
            status, _, body = post(conn, "/v1/gacha/get_series_list", {})
            first = json.loads(body)["gacha_series_list"][0]
            check("daily paid draw used up", not first["is_paid_enable"] and first["remaining_limited_paid_gacha_count"] == 0)
            # Birthday series follow the seven series open at the end of service, in start order. A
            # birthday series offers the standard cards released by its start plus its BR card; the BR
            # card (first record ends in 1) resolves to its own Museum record and can be exchanged.
            opened = json.loads((Path(__file__).resolve().parents[1] / "data" /
                                 "museum_gacha_opened_series.json").read_text(encoding="utf-8"))
            _, _, body = post(conn, "/v1/gacha/get_series_list", {})
            series_list = json.loads(body)["gacha_series_list"]
            ids = [s["gacha_series_id"] for s in series_list]
            check("gacha lists the closing seven, then the birthday series",
                  ids[:7] == [1055003, 1055002, 1055001, 1050511, 1050509, 1050506, 1050505] and
                  ids[7:] == [s["id"] for s in opened["series"]], str(ids))
            first_birthday = next(s for s in series_list if s["gacha_series_id"] == 1041031)
            last_birthday = series_list[-1]
            check("birthday pools grow with the release dates",
                  10319011 in first_birthday["card_list"] and
                  len(first_birthday["card_list"]) < len(last_birthday["card_list"]) <= 161, 
                  f"{len(first_birthday['card_list'])} {len(last_birthday['card_list'])}")
            drawn = set()
            draw_results = []
            for _ in range(10):  # 100 draws: the 100 exchange points the BR card costs
                status, _, body = post(conn, "/v1/gacha/set_purchase",
                                       {"gacha_series_id": 1041031, "gacha_set_purchase_type": 2, "campaign_id": 0})
                draw_results += json.loads(body).get("gacha_result_list", [])
                drawn |= {r["card_datas_id"] for r in json.loads(body).get("gacha_result_list", [])}
            # The Museum holds every card, so each draw is a duplicate: not NEW, no second copy, and
            # its series' limit-break material (LimitBreakMaterialRate) instead.
            with sqlite3.connect(db_path) as db:
                owned, series_owned = series_counts(db)
            check("drawing a held card adds no copy", owned == series_owned and
                  not any(r["is_new"] for r in draw_results), f"{owned}/{series_owned}")
            check("drawing a held card gives limit-break material",
                  any(r["material_limitbreak_item_id"] and r["material_limitbreak_num"] > 0 for r in draw_results),
                  json.dumps(draw_results[:2]))
            check("birthday draws stay in the pool", status == 200 and drawn and
                  drawn <= set(first_birthday["card_list"]), str(sorted(drawn - set(first_birthday["card_list"]))))
            status, _, body = post(conn, "/v1/gacha/set_guarantee_point_exchange",
                                   {"gacha_series_id": 1041031, "exchange_point_id": 1041031,
                                    "exchange_point_rate_id": 1001156, "exchange_num": 1})
            check("birthday BR card can be exchanged", status == 200 and
                  json.loads(body)["receipt_items"][0]["get_id"] == 10319011, body[:200].decode())
            with sqlite3.connect(db_path) as db:
                br = db.execute("SELECT card_datas_id FROM card_detail WHERE d_card_datas_id = "
                                "'00000000-0000-0000-0000-000010319011'").fetchone()
            check("BR base card has a Museum record", br == (10319011,), str(br))
            status, _, body = post(conn, "/v1/follow/search_id", {"player_id": "ABCDEFGHJ"})
            check("friend id search says not found", status == 200 and json.loads(body) == {"result_code": 1})
            # SHOW clear status: no fail state (the client shows LIVE CLEAR at MENTAL 0); AllPerfect when every note is Perfect+.
            def show_play(remain_mental, judgement):
                post(conn, "/v1/rhythm_game_live/set_start", {"music_id": 103101, "music_score_difficulty": 3})
                status, _, body = post(conn, "/v1/rhythm_game_live/set_finish", {
                    "score": 100, "remain_mental": remain_mental, "technical_score": 0, "enable_auto": False,
                    "notes_result_list": [{"judgement_result": judgement, "timing_msec": 0}] * 30, "play_data": ""})
                return status, json.loads(body)
            status, failed = show_play(0, 0)
            check("SHOW at MENTAL 0 is still Clear", status == 200 and failed["clear_status"] == 1, str(failed.get("clear_status")))
            check("SHOW at MENTAL 0 counted as clear", failed["class_mission_progress_list"][0]["progress_after"] ==
                  failed["class_mission_progress_list"][0]["progress_before"] + 1)
            status, perfect = show_play(100, 5)
            check("SHOW all perfect", perfect["clear_status"] == 3, str(perfect.get("clear_status")))
            mastery = perfect["music_mastery_level_result"]
            check("SHOW mastery owned with level exp", mastery["own_status"] == 2 and
                  mastery["music_mastery_exp_after"] > mastery["music_mastery_exp_before"], json.dumps(mastery))
            check("SHOW clear counted", perfect["class_mission_progress_list"][0]["progress_after"] ==
                  perfect["class_mission_progress_list"][0]["progress_before"] + 1)
            status, _, body = post(conn, "/v1/out_quest_live/get_stamina_recovery_info", {})
            info = json.loads(body)
            check("stamina recovery info", status == 200 and info["user_stamina"]["stamina_max"] == 999, body[:200].decode())
            status, _, body = post(conn, "/v1/out_quest_live/recovery_stamina", {"use_recovery_type": 1, "recovery_count": 1, "use_item_list": []})
            check("stamina refill", status == 200 and json.loads(body)["user_stamina"]["stamina_now"] >= 999, body[:200].decode())
            check("stamina hold limit is the 3-digit max", info.get("stamina_limit") == 999, json.dumps(info)[:200])
            status, _, body = post(conn, "/v1/out_quest_live/recovery_stamina",
                                   {"use_recovery_type": 2, "recovery_count": 1,
                                    "use_item_list": [{"item_id": 3001001, "item_num": 1}]})
            check("charge ticket cannot pass 999", status == 200 and
                  json.loads(body)["user_stamina"]["stamina_now"] == 999, body[:200].decode())
            # Retiring a quest live answers an object (the client stops on null) and gives the
            # プレイPt. spent at set_start back, once.
            def stamina_now() -> int:
                _, _, body = post(conn, "/v1/out_quest_live/get_stamina_recovery_info", {})
                return json.loads(body)["user_stamina"]["stamina_now"]
            with sqlite3.connect(db_path) as db:
                stage_id, cost = db.execute("SELECT stage_id, use_num FROM quest_stage WHERE use_num > 0 "
                                            "ORDER BY stage_id LIMIT 1").fetchone()
            _, _, body = post(conn, "/v1/out_quest_live/set_live_setting",
                              {"stage_id": stage_id, "deck_id": "", "music_id": 0, "quest_live_type": 1})
            live_id = json.loads(body).get("quest_live_id", "")
            post(conn, "/v1/in_quest_live/set_start", {"quest_live_id": live_id})
            spent = stamina_now()
            status, _, body = post(conn, "/v1/in_quest_live/set_retire",
                                   {"quest_live_id": live_id, "character_voices_id_list": []})
            retire = json.loads(body)
            check("quest live retire returns the spent プレイPt.",
                  status == 200 and retire == {"quest_live_type": 1, "return_resource": cost} and
                  stamina_now() == spent + cost == 999, body[:200].decode())
            _, _, body = post(conn, "/v1/in_quest_live/set_retire", {"quest_live_id": live_id})
            check("quest live retire pays back once", json.loads(body)["return_resource"] == 0 and
                  stamina_now() == 999, body[:200].decode())
            status, _, body = post(conn, "/v1/circle/get_detail", {"search_guild_key": ""})
            check("circle detail answers no circle", status == 200 and json.loads(body)["circle_info"] is None,
                  body[:200].decode())
            post(conn, "/v1/user/login", {"player_id": "ABCDEFGHJ", "device_specific_id": "", "version": 1})
            status, _, body = post(conn, "/v1/profile/get_info", {})
            check("login player id not echoed into profile",
                  json.loads(body)["profile_info"]["player_id"] == "000000000")
            status, _, body = post(conn, "/v1/profile/get_fan_level_info", {"player_id": profile["player_id"]})
            fan = json.loads(body)
            check("fan level info", status == 200 and len(fan["fan_level_info_list"]) == len(profile["fan_level_list"]) > 0,
                  body[:200].decode())
            # 103 Winter, first map: the square next to the start also lists the square after it,
            # so it must open when either neighbour is cleared (not only when both are).
            def grade_status(body: bytes) -> dict:
                return {s["grade_quest_square_id"]: s["status"] for s in json.loads(body)["quest_square_list"]}
            status, _, body = post(conn, "/v1/out_quest_live/grade/set_quest_start", {"grade_quest_series_id": 10341})
            squares = grade_status(body)
            check("grade quest start opens the first square", status == 200 and squares.get(31034101) == 1,
                  body[:300].decode())
            # Stepping on a live square does not clear it; passing its live does.
            post(conn, "/v1/out_quest_live/grade/set_quest_action", {"grade_quest_square_id": 31034101})
            status, _, body = post(conn, "/v1/out_quest_live/grade/set_quest_start", {"grade_quest_series_id": 10341})
            squares = grade_status(body)
            check("grade quest live square stays open until its live is passed",
                  squares.get(31034101) == 1 and squares.get(31034102) == 0, body[:300].decode())
            _, _, body = post(conn, "/v1/out_quest_live/set_live_setting",
                              {"stage_id": 531034101, "deck_id": "", "music_id": 0, "quest_live_type": 4,
                               "resource_value": 1})
            live_id = json.loads(body).get("quest_live_id", "")
            post(conn, "/v1/in_quest_live/set_start", {"quest_live_id": live_id})
            post(conn, "/v1/in_quest_live/set_finish", {"quest_live_id": live_id, "score": 100_000_000,
                                                        "play_report": ""})
            post(conn, "/v1/out_quest_live/grade/get_result", {})
            status, _, body = post(conn, "/v1/out_quest_live/grade/set_quest_start", {"grade_quest_series_id": 10341})
            squares = grade_status(body)
            check("grade quest live clear opens the next squares",
                  squares.get(31034101) == 2 and squares.get(31034102) == 1 and squares.get(61034101) == 1,
                  body[:300].decode())
            post(conn, "/v1/out_quest_live/grade/set_quest_retire", {"grade_quest_series_id": 10341})

            # Unlocks follow GradeQuestSeriesReleaseCond: SUMMER..WINTER open after 103 SPRING rank 2,
            # every GRADUATION (a single series numbered 6) after 103 SPRING rank 5.
            def grade_seasons() -> dict:
                _, _, body = post(conn, "/v1/out_quest_live/grade/get_quest_list", {})
                return {s["grade_quest_season_id"]: s for s in json.loads(body)["quest_season_list"]}

            def series_locked(seasons: dict, season_id: int) -> list:
                return [x["is_lock"] for x in seasons[season_id]["quest_series_list"]]

            def clear(*series_ids: int) -> None:
                with sqlite3.connect(db_path) as db:
                    db.executemany("INSERT OR REPLACE INTO grade_quest_progress(series_id,clear_status) VALUES(?,1)",
                                   [(i,) for i in series_ids])
            seasons = grade_seasons()
            check("grade quest: only 103 SPRING rank 1 open at start",
                  series_locked(seasons, 1031) == [False] + [True] * 5 and seasons[1034]["is_lock"] is True and
                  series_locked(seasons, 1035) == [True], json.dumps(series_locked(seasons, 1034)))
            clear(10311, 10312)
            seasons = grade_seasons()
            check("grade quest: SPRING rank 2 opens WINTER, not GRADUATION",
                  seasons[1034]["is_lock"] is False and series_locked(seasons, 1034) == [False] + [True] * 5 and
                  seasons[1054]["is_lock"] is False and series_locked(seasons, 1035) == [True])
            clear(10313, 10314, 10315)
            seasons = grade_seasons()
            check("grade quest: SPRING rank 5 opens every GRADUATION",
                  all(seasons[i]["is_lock"] is False and series_locked(seasons, i) == [False]
                      for i in (1035, 1045, 1055)), json.dumps(seasons[1035])[:300])

            # Grade Pt. builds up from GradeQuestRewards (a fresh player starts at Grade 1, 0 Pt).
            # Clearing 103 SPRING rank 1 meets all 5 rewards: 750 Grade Pt., Grade 5.
            def grade_detail() -> dict:
                _, _, body = post(conn, "/v1/out_quest_live/get_quest_top", {})
                return json.loads(body)["grade_detail"]

            def play(square: int, score: int) -> dict:
                with sqlite3.connect(db_path) as db:
                    squares = {s["grade_quest_square_id"]: s for row in db.execute(
                        "SELECT squares_json FROM grade_quest_series") for s in json.loads(row[0])}
                post(conn, "/v1/out_quest_live/grade/set_quest_action", {"grade_quest_square_id": square})
                if squares[square]["square_type"] not in (3, 7):
                    return {}
                _, _, body = post(conn, "/v1/out_quest_live/set_live_setting",
                                  {"stage_id": squares[square]["target_id"], "deck_id": "", "music_id": 0,
                                   "quest_live_type": 4, "resource_value": 1})
                live_id = json.loads(body).get("quest_live_id", "")
                post(conn, "/v1/in_quest_live/set_start", {"quest_live_id": live_id})
                post(conn, "/v1/in_quest_live/set_finish", {"quest_live_id": live_id, "score": score, "play_report": ""})
                _, _, body = post(conn, "/v1/out_quest_live/grade/get_result", {})
                return json.loads(body)
            with sqlite3.connect(db_path) as db:
                db.execute("DELETE FROM grade_quest_progress")
            detail = grade_detail()
            check("grade starts at 1 with 0 Pt", detail["grade_num"] == 1 and detail["total_grade_point"] == 0 and
                  {r["reward_status"] for r in detail["grade_rewards"]} == {1}, json.dumps(detail)[:200])
            post(conn, "/v1/out_quest_live/grade/set_quest_start", {"grade_quest_series_id": 10311})
            for square, score in ((21031101, 0), (31031101, 5_000_000), (41031101, 0), (31031102, 9_000_000),
                                  (41031102, 0)):
                play(square, score)
            goal = play(71031101, 50_000_000)["goal_clear_data"] or {}
            check("grade quest clear pays its rewards",
                  len(goal.get("clear_grade_quest_rewards_id_list", [])) == 5 and
                  goal.get("grade_up_data") == {"before_grade_num": 1, "after_grade_num": 5} and
                  goal.get("is_not_received_grade_reward") is True, json.dumps(goal)[:300])
            detail = grade_detail()
            reached = [r["grade_rewards_id"] for r in detail["grade_rewards"] if r["reward_status"] == 2]
            check("grade detail follows the earned Grade Pt", detail["total_grade_point"] == 750 and
                  detail["grade_num"] == 5 and reached == [100003, 100005], json.dumps(detail)[:300])
            _, _, body = post(conn, "/v1/out_quest_live/grade/set_reward", {})
            collected = {r["grade_rewards_id"]: r["reward_status"] for r in json.loads(body)["grade_rewards"]}
            check("set_reward collects reached Grade rewards", collected[100003] == collected[100005] == 3 and
                  collected[100010] == 1, body[:200].decode())

            # Replaying a cleared series pays nothing twice.
            post(conn, "/v1/out_quest_live/grade/set_quest_start", {"grade_quest_series_id": 10311})
            for square, score in ((21031101, 0), (31031101, 5_000_000), (41031101, 0), (31031102, 9_000_000),
                                  (41031102, 0)):
                play(square, score)
            goal = play(71031101, 50_000_000)["goal_clear_data"] or {}
            check("grade quest rewards pay once", goal.get("clear_grade_quest_rewards_id_list") == [] and
                  grade_detail()["total_grade_point"] == 750, json.dumps(goal)[:200])

            # Retiring pays the main live stage ライブPt.獲得率 rewards reached so far.
            with sqlite3.connect(db_path) as db:
                rate_rewards = db.execute("SELECT rewards_id, conditions_value FROM grade_quest_reward "
                                          "WHERE series_id=10312 AND conditions_type=1 ORDER BY order_id").fetchall()
                goal_stage = next(s for s in json.loads(db.execute(
                    "SELECT squares_json FROM grade_quest_series WHERE series_id=10312").fetchone()[0])
                    if s["square_type"] == 7)
                required = db.execute("SELECT live_point FROM grade_quest_stage WHERE stage_id=?",
                                      (goal_stage["target_id"],)).fetchone()[0]
            post(conn, "/v1/out_quest_live/grade/set_quest_start", {"grade_quest_series_id": 10312})
            post(conn, "/v1/out_quest_live/grade/set_quest_action", {"grade_quest_square_id": goal_stage["grade_quest_square_id"]})
            _, _, body = post(conn, "/v1/out_quest_live/set_live_setting",
                              {"stage_id": goal_stage["target_id"], "deck_id": "", "music_id": 0,
                               "quest_live_type": 4, "resource_value": 1})
            live_id = json.loads(body).get("quest_live_id", "")
            post(conn, "/v1/in_quest_live/set_start", {"quest_live_id": live_id})
            post(conn, "/v1/in_quest_live/set_finish", {"quest_live_id": live_id, "score": required * 1000 * 65 // 100,
                                                        "play_report": ""})
            post(conn, "/v1/out_quest_live/grade/get_result", {})
            _, _, body = post(conn, "/v1/out_quest_live/grade/set_quest_retire", {"grade_quest_series_id": 10312})
            retire = json.loads(body)
            expected = [rid for rid, value in rate_rewards if value <= 65]
            check("grade quest retire pays the reached live Pt. rate", expected and
                  retire["clear_grade_quest_rewards_id_list"] == expected, body[:300].decode())

            # Live Grand Prix: the Master's Grand Prix run in order from the first visit, each for its
            # original days. GP Pt. = score x stage rank bonus (no deck: no card bonus); the daily play
            # limit makes further plays rehearsals; an ended run is settled and the next one starts.
            def jst_day_start(offset: int = 0) -> str:
                day = (int(time.time()) + 5 * 3600) // 86400 + offset
                return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(day * 86400 - 5 * 3600))
            with sqlite3.connect(db_path) as db:
                first_gp, first_days = db.execute(
                    "SELECT grand_prix_id, duration_days FROM grand_prix ORDER BY order_id LIMIT 1").fetchone()
                quest_id, limit = db.execute("SELECT quest_id, play_limit_count FROM grand_prix_series "
                                             "WHERE grand_prix_id=? ORDER BY order_id LIMIT 1", (first_gp,)).fetchone()
                gp_stage = db.execute("SELECT stage_id, score1, score2, score3, bonus0, bonus1, bonus2, bonus3 "
                                      "FROM grand_prix_stage WHERE quest_id=? ORDER BY order_id DESC LIMIT 1",
                                      (quest_id,)).fetchone()
            _, _, body = post(conn, "/v1/out_quest_live/get_quest_top", {})
            top = json.loads(body)
            check("grand prix starts with the Master's first one",
                  top["grand_prix_id"] == first_gp and top["grand_prix_start_datetime"] == jst_day_start() and
                  top["grand_prix_end_datetime"] == jst_day_start(first_days), json.dumps(top)[:300])

            def gp_play(stage_id: int, score: int) -> dict:
                _, _, body = post(conn, "/v1/out_quest_live/set_live_setting",
                                  {"stage_id": stage_id, "deck_id": "", "music_id": 0, "quest_live_type": 6})
                live_id = json.loads(body).get("quest_live_id", "")
                post(conn, "/v1/in_quest_live/set_start", {"quest_live_id": live_id})
                post(conn, "/v1/in_quest_live/set_finish", {"quest_live_id": live_id, "score": score, "play_report": ""})
                _, _, body = post(conn, "/v1/out_quest_live/grand_prix/get_result", {"quest_live_id": live_id})
                return json.loads(body)
            stage_id, s1, s2, s3, b0, b1, b2, b3 = gp_stage
            score = s2 + 1  # reaches rank 2
            first = gp_play(stage_id, score)
            check("grand prix GP Pt. = score x stage rank bonus",
                  first["daily_score"] == score * b2 // 10000 and first["is_rehearsal"] is False, json.dumps(first)[:300])
            second = gp_play(stage_id, s3 + 1)
            check("grand prix plays past the daily limit are rehearsals",
                  limit != 1 or (second["daily_score"] == 0 and second["is_rehearsal"] is True), json.dumps(second)[:200])
            _, _, body = post(conn, "/v1/out_quest_live/grand_prix/get_stage_select", {"grand_prix_id": first_gp})
            select = json.loads(body)
            row = next(x for x in select["stages"] if x["quest_id"] == quest_id)
            check("grand prix stage select shows today's best and the limit",
                  row["daily_point"] == first["daily_score"] and row["play_count"] == min(2, limit or 99) and
                  row["playable_count"] == (limit or 99) and row["daily_rank"] > 0, json.dumps(row))
            _, _, body = post(conn, "/v1/out_quest_live/grand_prix/get_top_info", {"grand_prix_id": first_gp})
            info = json.loads(body)
            check("grand prix total and simulated rank",
                  info["personal_total_point"] == first["daily_score"] and 1 < info["personal_ranking"] < 999999 and
                  info["grand_prix_type"] == 1, json.dumps(info)[:300])
            # End the run: move the calendar back by its length.
            def item_num(item_id: int) -> int:
                with sqlite3.connect(db_path) as db:
                    row = db.execute("SELECT COALESCE(SUM(m.item_num),0) FROM museum_item_inventory m JOIN item i "
                                     "USING(d_item_datas_id) WHERE i.item_id=?", (item_id,)).fetchone()
                return row[0]
            with sqlite3.connect(db_path) as db:
                due = [r for row in db.execute(
                    "SELECT rewards_json FROM grand_prix_reward WHERE grand_prix_id=? AND min_rank<=? AND max_rank>=?",
                    (first_gp, info["personal_ranking"], info["personal_ranking"])) for r in json.loads(row[0])
                    if r["reward_type"] in (1, 3)]
                db.execute("UPDATE grand_prix_calendar SET epoch_day = epoch_day - ?", (first_days,))
                second_gp = db.execute("SELECT grand_prix_id FROM grand_prix ORDER BY order_id LIMIT 1 OFFSET 1").fetchone()[0]
            before = {r["item_id"]: item_num(r["item_id"]) for r in due}
            _, _, body = post(conn, "/v1/out_quest_live/get_quest_top", {})
            with sqlite3.connect(db_path) as db:
                settled = db.execute("SELECT grand_prix_id, total_point, personal_rank FROM grand_prix_settlement").fetchall()
            check("grand prix settles the ended run and starts the next",
                  settled == [(first_gp, first["daily_score"], info["personal_ranking"])] and
                  json.loads(body)["grand_prix_id"] == second_gp, f"{settled} {json.loads(body)['grand_prix_id']}")
            check("grand prix rank rewards are paid",
                  due and all(item_num(r["item_id"]) >= before[r["item_id"]] + r["num"] for r in due),
                  f"{due} {before}")

            # Profile ドリームスタイル所持数 = owned DR (rarity 8) cards.
            with sqlite3.connect(db_path) as db:
                dr_owned = db.execute("SELECT COUNT(*) FROM museum_card_ownership JOIN card_detail USING(d_card_datas_id) "
                                      "JOIN card_rarity r ON r.card_datas_id=card_detail.card_datas_id "
                                      "WHERE r.rarity=8").fetchone()[0]
            _, _, body = post(conn, "/v1/profile/get_info", {})
            check("profile dream_style_num counts DR cards",
                  dr_owned > 0 and json.loads(body)["profile_info"]["dream_style_num"] == dr_owned,
                  f"{dr_owned} {json.loads(body)['profile_info'].get('dream_style_num')}")
            # 006 raised GP PRIZE tickets to their 9999 cap once (the save was bootstrapped before this test set 123).
            with sqlite3.connect(db_path) as db:
                topped = db.execute("SELECT 1 FROM museum_bootstrap_state WHERE domain='GP_PRIZE_TICKET_TOPUP'").fetchone()
            check("GP PRIZE tickets topped up once", topped is not None)
            # 005: holdings above 999 (the membership tickets' 99999) are lowered once; point currencies keep theirs.
            check("five-digit holdings lowered to 999", item_num(3011003) == 999 and item_num(3004000) > 999,
                  f"{item_num(3011003)} {item_num(3004000)}")

            # Shop tiles and the per-player 解放Pt.変換 flag.
            _, _, body = post(conn, "/v1/shop/get_list", {})
            shops = json.loads(body)
            check("shop list serves the five shops",
                  sorted(s["shop_type"] for s in shops["shop_list"]) == [3, 7, 8, 9, 10] and
                  shops["is_convertible_limit_break_material"] is True, body[:300].decode())

            def card_owned(card_id: int) -> bool:
                with sqlite3.connect(db_path) as db:
                    return db.execute("SELECT 1 FROM museum_card_ownership JOIN card_detail USING(d_card_datas_id) "
                                      "WHERE card_datas_id/10=?/10", (card_id,)).fetchone() is not None

            def drop_card(card_id: int) -> None:
                with sqlite3.connect(db_path) as db:
                    db.execute("PRAGMA foreign_keys=ON")
                    db.execute("DELETE FROM museum_card_ownership WHERE d_card_datas_id IN "
                               "(SELECT d_card_datas_id FROM card_detail WHERE card_datas_id/10=?/10)", (card_id,))

            def set_item(item_id: int, num: int) -> None:
                with sqlite3.connect(db_path) as db:
                    db.execute("UPDATE museum_item_inventory SET item_num=? WHERE d_item_datas_id="
                               "(SELECT d_item_datas_id FROM item WHERE item_id=?)", (num, item_id))

            # GP PRIZE交換所: 200 tickets for a card; a held card turns into its limit-break material.
            gp_rows = yaml_rows("GpPrizeExchanges.yaml")
            _, _, body = post(conn, "/v1/gp_prize_exchange/get_list", {})
            gp_list = json.loads(body)
            prize = gp_rows[0]
            set_item(prize["MaterialItemId"], 500)
            drop_card(prize["ProductItemId"])
            status, _, body = post(conn, "/v1/gp_prize_exchange/set_purchase", {"gp_prize_exchanges_id": prize["Id"]})
            check("GP PRIZE exchange pays a card",
                  len(gp_list["gp_prize_exchange_list"]) == len(gp_rows) and status == 200 and
                  json.loads(body) == {"gp_prize_ticket_id": prize["MaterialItemId"], "user_ticket_num": 300} and
                  card_owned(prize["ProductItemId"]) and item_num(prize["MaterialItemId"]) == 300, body[:200].decode())
            series = {c["Id"]: c["CardSeriesId"] for c in yaml_rows("CardDatas.yaml")}[prize["ProductItemId"]]
            material = next(r for r in yaml_rows("LimitBreakMaterialRate.yaml") if r["CardSeriesId"] == series)
            held = item_num(material["LimitBreakMaterialId"])
            status, _, body = post(conn, "/v1/gp_prize_exchange/set_purchase", {"gp_prize_exchanges_id": prize["Id"]})
            check("GP PRIZE duplicate becomes limit-break material",
                  status == 200 and item_num(material["LimitBreakMaterialId"]) ==
                  held + material["LimitBreakMaterialQuantity"], f"{held} {item_num(material['LimitBreakMaterialId'])}")

            # ステッカー交換所: sticker points for a sticker not held, once.
            sticker = yaml_rows("StickerExchanges.yaml")[0]
            set_item(sticker["MaterialItemId"], 100)
            with sqlite3.connect(db_path) as db:
                db.execute("DELETE FROM museum_sticker_ownership WHERE stickers_id=?", (sticker["ProductStickerId"],))
            _, _, body = post(conn, "/v1/sticker_exchange/get_list", {})
            listed = {e["sticker_exchanges_id"]: e["is_having"] for e in json.loads(body)["sticker_exchange_list"]}
            status, _, body = post(conn, "/v1/sticker_exchange/set_purchase", {"sticker_exchanges_id": sticker["Id"]})
            again, _, _ = post(conn, "/v1/sticker_exchange/set_purchase", {"sticker_exchanges_id": sticker["Id"]})
            check("sticker exchange buys a missing sticker once",
                  listed.get(sticker["Id"]) is False and status == 200 and
                  json.loads(body) == {"sticker_point_num": 100 - sticker["MaterialItemNum"]} and again == 400,
                  f"{listed.get(sticker['Id'])} {status} {body[:100]} {again}")

            # SIsCaストア: free SIsCa first; paid-only items take paid SIsCa.
            with sqlite3.connect(db_path) as db:
                db.execute("UPDATE museum_gacha_state SET free_jewels=100, paid_jewels=5000 WHERE series_id=1055003")
            _, _, body = post(conn, "/v1/sisca_store/get_list", {})
            store = {e["sisca_store_datas_id"] for e in json.loads(body)["item_list"]}
            mini = item_num(2004002)
            status, headers, body = post(conn, "/v1/sisca_store/set_purchase", {"sisca_store_datas_id": 5, "purchase_num": 2})
            bought = json.loads(body)
            check("SIsCa store spends free then paid SIsCa",
                  store == {1, 2, 3, 4, 5} and status == 200 and bought["sisca_info"] == {"free": 0, "paid": 4940} and
                  bought["user_item_info"]["item_num"] == mini + 2 == item_num(2004002) and
                  "jewel_free: 0" in "\n".join(f"{k}: {v}" for k, v in headers.items()), body[:300].decode())
            status, _, body = post(conn, "/v1/sisca_store/set_purchase", {"sisca_store_datas_id": 1, "purchase_num": 1})
            check("SIsCa store paid-only item", status == 200 and json.loads(body)["sisca_info"] == {"free": 0, "paid": 3940},
                  body[:200].decode())

            # セレクトチケット: held tickets are listed; one ticket picks one card.
            _, _, body = post(conn, "/v1/gacha/get_series_list", {})
            check("select tickets stay off the gacha screen", json.loads(body)["select_ticket_series_list"] == [])
            _, _, body = post(conn, "/v1/select_ticket_exchange/get_list", {})
            shop_tickets = {e["select_ticket_series_id"]: e["user_select_ticket_num"]
                            for e in json.loads(body)["select_ticket_exchange_list"]}
            ticket_series = next(r for r in yaml_rows("SelectTicketSeries.yaml") if r["Id"] == 4030036)
            rate = next(r for r in yaml_rows("SelectTicketExchangeRate.yaml") if r["SelectTicketSeriesId"] == 4030036)
            tickets = item_num(ticket_series["ExchangeTicketId"])
            # The 選択 button first lists the ticket's cards (GachaExchangeType 2 = SelectTicket).
            status, _, body = post(conn, "/v1/gacha/get_exchange_card_having_list",
                                   {"exchange_type": 2, "exchange_id": 4030036})
            listed = json.loads(body)["exchange_card_list"] if status == 200 else []
            master_cards = {r["ExchangeItemId"] for r in yaml_rows("SelectTicketExchangeRate.yaml")
                            if r["SelectTicketSeriesId"] == 4030036}
            check("select ticket card list", status == 200 and {c["card_datas_id"] for c in listed} == master_cards and
                  all(c["card_name"] and c["card_rarity"] for c in listed) and all(c["is_having"] for c in listed),
                  body[:200].decode())
            status, _, body = post(conn, "/v1/gacha/get_exchange_card_having_list",
                                   {"exchange_type": 1, "exchange_id": 1021022})
            check("guarantee point card list", status == 200 and len(json.loads(body)["exchange_card_list"]) >= 1,
                  body[:200].decode())
            drop_card(rate["ExchangeItemId"])
            status, _, body = post(conn, "/v1/gacha/set_select_ticket_exchange",
                                   {"select_ticket_exchange_rate_id": rate["Id"]})
            check("select tickets listed in the shop", len(shop_tickets) >= 50 and shop_tickets.get(4030036) == tickets,
                  f"{len(shop_tickets)}")
            check("select ticket exchange pays the chosen card",
                  status == 200 and [(r["get_item_type"], r["get_id"] // 10, r["get_num"], r["is_new"])
                                     for r in json.loads(body)["receipt_items"]] ==
                  [(2, rate["ExchangeItemId"] // 10, 1, True)] and
                  card_owned(rate["ExchangeItemId"]) and item_num(ticket_series["ExchangeTicketId"]) == tickets - 1,
                  body[:200].decode())

            # 解放Pt.変換: materials beyond the remaining limit breaks, 25 of a rarity -> one ティア.
            _, _, body = post(conn, "/v1/item_exchange/get_limit_break_material_convert_list", {})
            convert = {e["rarity"]: e["limit_break_material_num"]
                       for e in json.loads(body)["convert_limit_break_material_list"]}
            rates = {r["LimitBreakMaterialRarity"]: r for r in yaml_rows("LimitBreakMaterialConvertRate.yaml")}
            item_limits = {r["Id"]: r["LimitNum"] for r in yaml_rows("Items.yaml")}
            expected = {}
            for rarity, num in convert.items():
                product = rates[rarity]["ProductItemId"]
                expected[product] = expected.get(product, 0) + num // rates[rarity]["LimitBreakMaterialNum"]
            tiers_before = {item: item_num(item) for item in expected}
            status, _, body = post(conn, "/v1/item_exchange/set_limit_break_material_convert", {})
            got = {e["item_id"]: e["get_num"] for e in json.loads(body)["get_item_list"]}
            _, _, after = post(conn, "/v1/shop/get_list", {})
            # The Museum's 999 of every material converts until a ティア reaches its 9999 holding cap.
            check("limit-break materials convert into tiers up to the holding cap",
                  convert and all(n % rates[r]["LimitBreakMaterialNum"] == 0 for r, n in convert.items()) and
                  status == 200 and got == expected and
                  all(item_num(k) == tiers_before[k] + v <= item_limits[k] for k, v in got.items()) and
                  json.loads(after)["is_convertible_limit_break_material"] is False,
                  f"{convert} {got} {expected}")
            conn.close()
        finally:
            log += stop(proc)
        check("no backend errors logged", "[E]" not in log, log[-400:])

    # Archive covers: a file under --asset-root at the CDN path is served from /a/ and the
    # archive list points at it; covers without a local file keep their CDN URL.
    with tempfile.TemporaryDirectory() as tmp:
        data, assets = Path(tmp) / "data", Path(tmp) / "assets"
        data.mkdir()
        archive = json.loads((Path(__file__).resolve().parents[1] / "data" / "builtin" /
                              "archive_get_archive_list.json").read_text(encoding="utf-8"))["archive_list"]
        cdn = "https://assets.link-like-lovelive.app/"
        local_rel = archive[1]["thumbnail_image_url"][len(cdn):]
        (assets / local_rel).parent.mkdir(parents=True)
        (assets / local_rel).write_bytes(b"\xff\xd8\xff\xe0test-cover")
        port = free_port()
        proc = start(data, port, "--asset-root", str(assets))
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
            post(conn, "/v1/user/login", {"player_id": "", "device_specific_id": "", "version": 1})
            _, _, body = post(conn, "/v1/archive/get_archive_list", {})
            urls = [a["thumbnail_image_url"] for a in json.loads(body)["archive_list"]]
            check("archive cover with a local file points at /a/",
                  urls[1] == f"http://127.0.0.1:{port}/a/{local_rel}" and
                  all(u.startswith(cdn) for i, u in enumerate(urls) if i != 1), str(urls[:3]))
            conn.request("GET", f"/a/{local_rel}")
            reply = conn.getresponse()
            check("local archive cover is served as JPEG",
                  reply.status == 200 and reply.getheader("content-type") == "image/jpeg" and
                  reply.read() == b"\xff\xd8\xff\xe0test-cover")
            # The list has every archive, newest first, and pages by offset/limit (a
            # static body used to repeat the same page forever).
            ids = [a["archives_id"] for a in json.loads(body)["archive_list"]]
            _, _, first = post(conn, "/v1/archive/get_archive_list", {"offset": 0, "limit": 20})
            _, _, second = post(conn, "/v1/archive/get_archive_list", {"offset": 20, "limit": 20})
            _, _, past = post(conn, "/v1/archive/get_archive_list", {"offset": len(ids), "limit": 20})
            page1 = [a["archives_id"] for a in json.loads(first)["archive_list"]]
            page2 = [a["archives_id"] for a in json.loads(second)["archive_list"]]
            check("archive list pages through all 398 archives",
                  len(ids) == 398 and len(set(ids)) == 398 and page1 == ids[:20] and page2 == ids[20:40] and
                  json.loads(past)["archive_list"] == [], f"{len(ids)} {len(page1)} {len(page2)}")
            # A motion-capture replay the official detail had no archive_url for (an mrs
            # recording kept by the community mirror) gets its entry from 008.
            _, _, body = post(conn, "/v1/archive/get_with_archive_data",
                              {"archives_id": "025816e3-0c7b-4fa3-b552-423a770730a7"})
            replay = json.loads(body).get("archive_url", "")
            check("mrs replay archive_url filled from the mirror list",
                  replay.startswith(cdn + "archive/mrs/") and replay.endswith(".iarc"), replay)
            # With×MEETS replays poll withlive_info (GET) for comments and the AFTER state.
            conn.request("GET", "/v1/archive/withlive_info?live_id=a6fe0908-6dcc-4774-a6ff-70f39a6d7911")
            reply = conn.getresponse()
            info = json.loads(reply.read() or b"{}")
            check("archive withlive_info answers the replay's polling",
                  reply.status == 200 and info.get("polling_interval") == 60 and info.get("timelines") == [],
                  f"{reply.status} {info}")
            # With×MEETS plays video whenever video_url is present; motion replays drop it.
            _, _, body = post(conn, "/v1/archive/get_with_archive_data",
                              {"archives_id": "a6fe0908-6dcc-4774-a6ff-70f39a6d7911"})
            detail = json.loads(body)
            check("With×MEETS motion replay has no video_url",
                  detail.get("archive_url", "").endswith(".md") and "video_url" not in detail, str(detail.get("video_url")))
            conn.close()
        finally:
            stop(proc)


    # Remote asset base (R2): the proxy sends the device key, fetches large assets in
    # Range pieces and streams them on, honours the client's Range, answers misses with
    # a bodiless 404, and takes x-res-version from the published version when given.
    import os
    import threading
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    big = os.urandom(1_000_003)
    small = b"small-asset"
    upstream_log: list = []

    class Upstream(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):  # noqa: N802
            upstream_log.append((self.path, self.headers.get("Range"), self.headers.get("x-linkura-key")))
            if self.headers.get("x-linkura-key") != "device-key":
                self.send_response(403)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            data = {"/a/raw/bi/big": big, "/a/raw/sm/small": small}.get(self.path)
            if data is None:
                body = b"<html>Not Found</html>"
                self.send_response(404)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            first, last, status = 0, len(data) - 1, 200
            rng = self.headers.get("Range")
            if rng and rng.startswith("bytes="):
                a, _, b = rng[6:].partition("-")
                first = int(a)
                last = min(int(b), len(data) - 1) if b else len(data) - 1
                status = 206
            self.send_response(status)
            self.send_header("Content-Length", str(last - first + 1))
            if status == 206:
                self.send_header("Content-Range", f"bytes {first}-{last}/{len(data)}")
            self.end_headers()
            self.wfile.write(data[first:last + 1])

    upstream = ThreadingHTTPServer(("127.0.0.1", 0), Upstream)
    threading.Thread(target=upstream.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory() as tmp:
        data = Path(tmp) / "data"
        version_file = Path(tmp) / "version.txt"
        version_file.write_text("R9999999@remote-one\n")
        port = free_port()
        proc = start(data, port, "--res-version", "R0000000@configured", "--motion-replays", "off",
                     "--remote-asset-base", f"http://127.0.0.1:{upstream.server_port}/a",
                     "--remote-asset-key", "device-key", "--remote-chunk-bytes", "65536",
                     "--res-version-file", str(version_file))
        try:
            def get(path: str, headers: dict | None = None):
                c = http.client.HTTPConnection("127.0.0.1", port, timeout=60)
                c.request("GET", path, headers=headers or {})
                r = c.getresponse()
                out = (r.status, dict((k.lower(), v) for k, v in r.getheaders()), r.read())
                c.close()
                return out

            upstream_log.clear()
            status, headers, body = get("/a/raw/bi/big")
            pieces = [r for p_, r, _ in upstream_log if p_ == "/a/raw/bi/big"]
            sizes = [int(r[6:].split("-")[1]) - int(r[6:].split("-")[0]) + 1 for r in pieces if r]
            check("proxied asset streams in Range pieces with the device key",
                  status == 200 and body == big and len(pieces) == 16 and max(sizes) <= 65536 and
                  all(k == "device-key" for _, _, k in upstream_log), f"{status} {len(body)} {len(pieces)}")
            status, headers, body = get("/a/raw/bi/big", {"Range": "bytes=100000-300000"})
            check("proxied asset honours the client's Range",
                  status == 206 and body == big[100000:300001] and
                  headers.get("content-range") == f"bytes 100000-300000/{len(big)}", f"{status} {headers}")
            status, headers, body = get("/a/raw/bi/big", {"Range": "bytes=900000-"})
            check("proxied asset honours an open-ended Range", status == 206 and body == big[900000:])
            status, _, body = get("/a/raw/sm/small")
            check("small proxied asset comes back whole", status == 200 and body == small)
            status, _, body = get("/a/raw/no/missing")
            check("remote miss is a bodiless 404", status == 404 and body == b"")
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
            _, first_headers, _ = post(conn, "/v1/user/login", {"player_id": "", "device_specific_id": "", "version": 1})
            version_file.write_text("R9999999@remote-two\n")
            _, second_headers, _ = post(conn, "/v1/user/login", {"player_id": "", "device_specific_id": "", "version": 1})
            lower = lambda h: {k.lower(): v for k, v in h.items()}
            check("x-res-version follows the published version",
                  lower(first_headers).get("x-res-version") == "R9999999@remote-one" and
                  lower(second_headers).get("x-res-version") == "R9999999@remote-two",
                  f"{lower(first_headers).get('x-res-version')} {lower(second_headers).get('x-res-version')}")
            # With×MEETS mrs replays play the video replay (009).
            _, _, body = post(conn, "/v1/archive/get_with_archive_data",
                              {"archives_id": "e77f7985-6a67-4a59-a689-d4870c96cf03"})
            detail = json.loads(body)
            _, _, body = post(conn, "/v1/archive/get_with_archive_data",
                              {"archives_id": "a6fe0908-6dcc-4774-a6ff-70f39a6d7911"})
            detail = json.loads(body)
            check("with motion replays off an als With×MEETS archive plays video",
                  "archive_url" not in detail and "/a/archive/hls/" in detail.get("video_url", ""),
                  str({k: detail.get(k) for k in ("archive_url", "video_url")}))
            _, _, body = post(conn, "/v1/archive/get_with_archive_data",
                              {"archives_id": "e77f7985-6a67-4a59-a689-d4870c96cf03"})
            detail = json.loads(body)
            check("With×MEETS mrs archive plays its video replay",
                  "archive_url" not in detail and detail.get("video_url", "").endswith("/archive/hls/20241107/index.m3u8"),
                  str({k: detail.get(k) for k in ("archive_url", "video_url")}))
            conn.close()
        finally:
            stop(proc)
            upstream.shutdown()

    print("PASS" if not failures else f"FAILED: {', '.join(failures)}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
