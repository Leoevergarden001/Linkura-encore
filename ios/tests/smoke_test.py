#!/usr/bin/env python3
"""Smoke test for the offline API server (host build).

    ios/build.sh host && python3 ios/tests/smoke_test.py

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
        print(f"missing {BINARY}; run ios/build.sh host first")
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
            status, _, body = post(conn, "/v1/profile/get_fan_level_info", {"player_id": profile["player_id"]})
            fan = json.loads(body)
            check("fan level info", status == 200 and len(fan["fan_level_info_list"]) == len(profile["fan_level_list"]) > 0,
                  body[:200].decode())
            conn.close()
        finally:
            log += stop(proc)
        check("no backend errors logged", "[E]" not in log, log[-400:])

    print("PASS" if not failures else f"FAILED: {', '.join(failures)}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
