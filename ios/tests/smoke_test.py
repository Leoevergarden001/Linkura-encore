#!/usr/bin/env python3
"""Smoke test for the offline API server (host build).

    ios/build.sh host && python3 ios/tests/smoke_test.py

Starts build/host/linkura-offline-server on a free port with a temporary
data directory and checks the HTTP framing the game's .NET HttpClient uses
(keep-alive, Expect: 100-continue, chunked request bodies) plus a few routes.
"""
from __future__ import annotations

import http.client
import json
import socket
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
            conn.close()
        finally:
            log += stop(proc)
        check("no backend errors logged", "[E]" not in log, log[-400:])

    print("PASS" if not failures else f"FAILED: {', '.join(failures)}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
