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


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def start(data: Path, port: int) -> subprocess.Popen:
    proc = subprocess.Popen([str(BINARY), "--data", str(data), "--port", str(port)],
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

        # Restart on the same data directory: state persists, no re-bootstrap errors.
        proc = start(data, port)
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
            status, _, _ = post(conn, "/v1/home/get_home", {})
            check("home after restart", status == 200)

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

            # Archive / circle screens that used to 404.
            status, _, body = post(conn, "/v1/archive/get_ranking_list", {"member_id": 1021, "target_period": 1})
            ranking = json.loads(body)
            check("archive ranking", status == 200 and ranking["my_rank_status"] == 3 and
                  len(ranking["cheer_rewards"]) > 0 and ranking["ranking_start_time"].startswith("2026-04-02"), body[:200].decode())
            status, _, body = post(conn, "/v1/archive/get_with_station_list", {"limit": 20, "offset": 0})
            stations = json.loads(body)
            check("with station list", status == 200 and stations["with_station_list"] == [] and
                  len(stations["filterable_characters"]) > 0, body[:200].decode())
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
            first_card = json.loads(body)["user_card_data_list"][0]["d_card_datas_id"]
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
            for _ in range(10):  # 100 draws: the 100 exchange points the BR card costs
                status, _, body = post(conn, "/v1/gacha/set_purchase",
                                       {"gacha_series_id": 1041031, "gacha_set_purchase_type": 2, "campaign_id": 0})
                drawn |= {r["card_datas_id"] for r in json.loads(body).get("gacha_result_list", [])}
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
            conn.close()
        finally:
            log += stop(proc)
        check("no backend errors logged", "[E]" not in log, log[-400:])

    print("PASS" if not failures else f"FAILED: {', '.join(failures)}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
