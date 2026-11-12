#!/usr/bin/env python3
"""Set up the Linkura offline iOS client on a device from a local backup.

    python3 tools/ios_device_setup.py --backup <backup dir> --device "iPhone 14 Pro" \
        [--unsigned client.ipa | --ipa signed.ipa] [--launch]

A copy of this script (with ios_sign_ipa.sh) inside the backup's tools/
folder finds the backup and its client/*-unsigned.ipa on its own.

Steps: find the device -> (sign) -> install -> push the asset cache, master
data and storage key from the backup -> verify by listing the device -> launch.

The backup is the directory written by the iOS backup (``container/Documents``
and ``container/Library`` mirror the app's data container). Only the files the
client needs are pushed; debug files and ``._*`` AppleDouble files are skipped.
Re-running is safe: files already on the device with the same size are
skipped, files whose size differs are left alone unless ``--overwrite``.

The player database (progress) is only pushed with ``--include-progress`` and
never replaces an existing one unless ``--overwrite-progress``, which first
pulls the device copy to a local folder.

The backup holds account-bound material (storage key); keep it private.
Needs Xcode (``xcrun devicectl``); the device must be paired, unlocked and in
Developer Mode. Free (Personal Team) signatures expire after 7 days.
"""
from __future__ import annotations

import argparse
import datetime as dt
import html
import json
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import zipfile
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

DEFAULT_BUNDLE = "org.linkura.preservation.probe"
REPO = Path(__file__).resolve().parent.parent
SIGNER = REPO / "tools" / "ios_sign_ipa.sh"
PROFILE_DIRS = [
    Path.home() / "Library/Developer/Xcode/UserData/Provisioning Profiles",
    Path.home() / "Library/MobileDevice/Provisioning Profiles",
]
JUNK_NAMES = {".DS_Store"}
# Paths under container/ that the client needs (relative to the container).
REQUIRED_TREES = ["Documents/D", "Documents/M", "Documents/LinkuraOffline/assets"]
REQUIRED_FILES = ["Documents/LinkuraOffline/storage-key.json"]
OPTIONAL_FILES = ["Documents/LinkuraOffline/config.json"]
PROGRESS_FILES = [
    "Library/Application Support/LinkuraOffline/HasuKikaisann.sqlite3",
    "Library/Application Support/LinkuraOffline/rhythm_game_home.json",
]
PROGRESS_DB = PROGRESS_FILES[0]
RETRIES = 3
# Asset cache: content-addressed names, the backup copy is authoritative.
CACHE_PREFIXES = ("Documents/D/", "Documents/LinkuraOffline/assets/")


class SetupError(Exception):
    pass


class DeviceFull(SetupError):
    pass


def log(msg: str = "") -> None:
    print(msg, flush=True)


def gb(n: int) -> str:
    return f"{n / 1e9:.2f} GB"


def is_junk(name: str) -> bool:
    return name.startswith("._") or name in JUNK_NAMES


# --------------------------------------------------------------------------- devicectl

def devicectl(args: list[str], *, timeout: int | None = None, want_json: bool = True) -> dict:
    """Run `xcrun devicectl ...` and return its JSON `result`."""
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "out.json"
        cmd = ["xcrun", "devicectl", *args, "--quiet"]
        if want_json:
            cmd += ["--json-output", str(out)]
        if timeout:
            cmd += ["--timeout", str(timeout)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        data = json.loads(out.read_text()) if want_json and out.exists() else {}
        if proc.returncode != 0:
            err = data.get("error", {}).get("userInfo", {}) if data else {}
            desc = (err.get("NSLocalizedDescription", {}) or {}).get("string") if isinstance(err, dict) else None
            tail = (proc.stderr or proc.stdout).strip().splitlines()[-3:]
            raise SetupError(f"devicectl {' '.join(args[:3])} 失败: {desc or ' / '.join(tail)}")
        return data.get("result", {})


@dataclass
class Device:
    udid: str
    name: str
    model: str
    os_version: str
    connected: bool
    developer_mode: str
    capacity: int | None


def list_devices() -> list[Device]:
    devices = []
    for d in devicectl(["list", "devices"]).get("devices", []):
        hw, dp = d.get("hardwareProperties", {}), d.get("deviceProperties", {})
        if hw.get("platform") != "iOS" or hw.get("reality") == "virtual":
            continue
        devices.append(Device(
            udid=hw.get("udid") or d.get("identifier"),
            name=dp.get("name", "?"),
            model=hw.get("marketingName", "?"),
            os_version=dp.get("osVersionNumber", "?"),
            connected=d.get("connectionProperties", {}).get("tunnelState") == "connected",
            developer_mode=dp.get("developerModeStatus", "?"),
            capacity=hw.get("internalStorageCapacity"),
        ))
    return devices


def pick_device(query: str | None) -> Device:
    devices = list_devices()
    if query:
        q = query.lower()
        hits = [d for d in devices if q in (d.udid.lower(), d.name.lower())] or \
               [d for d in devices if q in d.name.lower() or q in d.model.lower()]
    else:
        hits = [d for d in devices if d.connected]
    if len(hits) != 1:
        log("可用设备：")
        for d in devices:
            log(f"  {'已连接' if d.connected else '未连接'}  {d.name}  ({d.model}, iOS {d.os_version})  {d.udid}")
        raise SetupError("没有找到唯一的设备，请用 --device 指定名称或 UDID" if hits else
                         "没有匹配的设备（需要先用数据线连接并在设备上点“信任”）")
    dev = hits[0]
    if not dev.connected:
        raise SetupError(f"{dev.name} 已配对但当前未连接：请用数据线连接、解锁屏幕后重试")
    if dev.developer_mode not in ("enabled", "?"):
        raise SetupError(f"{dev.name} 未开启开发者模式：设置 → 隐私与安全性 → 开发者模式，开启后重启设备")
    return dev


def device_files(dev: Device, bundle: str, subdir: str) -> dict[str, int]:
    """relativePath (from the container root) -> size, for files under `subdir`."""
    try:
        res = devicectl(["device", "info", "files", "--device", dev.udid,
                         "--domain-type", "appDataContainer", "--domain-identifier", bundle,
                         "--subdirectory", subdir], timeout=900)
    except SetupError as e:
        if "No such file" in str(e) or "doesn’t exist" in str(e) or "does not exist" in str(e):
            return {}
        raise
    out = {}
    for f in res.get("files", []):
        if f.get("resources", {}).get("isDirectory"):
            continue
        out[f"{subdir}/{f['relativePath']}"] = int(f.get("metadata", {}).get("size", -1))
    return out


def installed_app(dev: Device, bundle: str) -> dict | None:
    apps = devicectl(["device", "info", "apps", "--device", dev.udid, "--bundle-id", bundle]).get("apps", [])
    return apps[0] if apps else None


def terminate_app(dev: Device, app: dict | None) -> None:
    if not app:
        return
    url = app.get("url", "").rstrip("/")
    procs = devicectl(["device", "info", "processes", "--device", dev.udid]).get("runningProcesses", [])
    for p in procs:
        if url and p.get("executable", "").startswith(url + "/"):
            log(f"  结束正在运行的客户端 (pid {p['processIdentifier']})")
            devicectl(["device", "process", "terminate", "--device", dev.udid,
                       "--pid", str(p["processIdentifier"])], want_json=False)
            time.sleep(2)


# devicectl's launch error when iOS has not trusted the signing developer yet
# (first app from this identity on the device).
UNTRUSTED_MARKERS = ("explicitly trusted", "FBSOpenApplicationErrorDomain error 3")
TRUST_STEPS = "设置 → 通用 → VPN 与设备管理，在“开发者 App”下点签名身份并信任"
UNTRUSTED_DEVELOPER = ("{name} 还没有信任这个开发者，客户端无法启动：在设备上打开 " + TRUST_STEPS +
                       "。然后重跑同一条命令即可（覆盖安装保留数据，已推送的文件会跳过），"
                       "或者把 --unsigned/--ipa 换成 --skip-install 只启动和校验。")


def launch_app(dev: "Device", bundle: str) -> None:
    try:
        devicectl(["device", "process", "launch", "--device", dev.udid, "--terminate-existing", bundle],
                  want_json=False)
    except SetupError as e:
        if any(m in str(e) for m in UNTRUSTED_MARKERS):
            raise SetupError(UNTRUSTED_DEVELOPER.format(name=dev.name)) from None
        raise


BACKUP_GONE = ("读不到备份文件 {src}：备份盘可能断开了。重新插好（最好直接接 Mac、换根线或换个口），"
               "再运行同一条命令，已经推送的文件会跳过")


def push(dev: Device, bundle: str, src: Path, dest: str) -> None:
    last = None
    for attempt in range(1, RETRIES + 1):
        if not src.exists():
            raise SetupError(BACKUP_GONE.format(src=src))
        try:
            devicectl(["device", "copy", "to", "--device", dev.udid,
                       "--domain-type", "appDataContainer", "--domain-identifier", bundle,
                       "--source", str(src), "--destination", dest], timeout=3600, want_json=False)
            return
        except SetupError as e:
            last = e
            if not src.exists():
                raise SetupError(BACKUP_GONE.format(src=src)) from None
            if "error 28 " in str(e) or "No space left" in str(e):
                raise DeviceFull(str(e)) from None
            log(f"    第 {attempt} 次推送失败：{e}（保持设备解锁、亮屏）")
            time.sleep(3 * attempt)
    raise SetupError(f"推送 {dest} 失败：{last}")


def pull(dev: Device, bundle: str, src: str, dest: Path) -> None:
    devicectl(["device", "copy", "from", "--device", dev.udid,
               "--domain-type", "appDataContainer", "--domain-identifier", bundle,
               "--source", src, "--destination", str(dest)], timeout=600, want_json=False)


# --------------------------------------------------------------------------- IPA / signing

def read_profile(path: Path) -> dict:
    raw = subprocess.run(["security", "cms", "-D", "-i", str(path)], capture_output=True).stdout
    return plistlib.loads(raw) if raw else {}


def profile_covers(profile: dict, bundle: str, udid: str) -> bool:
    app_id = profile.get("Entitlements", {}).get("application-identifier", "")
    if not (app_id.endswith("." + bundle) or app_id.endswith(".*")):
        return False
    if profile.get("ExpirationDate") and profile["ExpirationDate"] <= dt.datetime.now(dt.timezone.utc).replace(tzinfo=None):
        return False
    return profile.get("ProvisionsAllDevices") or udid in profile.get("ProvisionedDevices", [])


def find_profile(bundle: str, udid: str) -> Path:
    best = None
    for d in PROFILE_DIRS:
        for p in sorted(d.glob("*.mobileprovision")) if d.is_dir() else []:
            prof = read_profile(p)
            if profile_covers(prof, bundle, udid) and (best is None or prof["ExpirationDate"] > best[1]):
                best = (p, prof["ExpirationDate"])
    if not best:
        raise SetupError(
            f"找不到包含该设备、Bundle ID 为 {bundle} 且未过期的描述文件。\n"
            "  在 Xcode 里新建任意 App，Bundle Identifier 填这个 ID、Team 选你的账号，"
            "连接该设备后运行一次，Xcode 会自动登记设备并生成描述文件；也可以用 --profile 指定。")
    log(f"  描述文件：{best[0].name}（有效期到 {best[1]:%Y-%m-%d %H:%M} UTC）")
    return best[0]


def find_identity() -> str:
    out = subprocess.run(["security", "find-identity", "-v", "-p", "codesigning"],
                         capture_output=True, text=True).stdout
    ids = [line.split('"')[1] for line in out.splitlines() if '"' in line]
    dev_ids = [i for i in ids if i.startswith("Apple Development")] or ids
    if len(dev_ids) != 1:
        raise SetupError("签名身份不唯一或不存在，请用 --identity 指定（security find-identity -v -p codesigning）")
    return dev_ids[0]


def ipa_info(ipa: Path) -> tuple[str, list[str] | None]:
    """(bundle id, provisioned device UDIDs or None if unsigned)."""
    with zipfile.ZipFile(ipa) as z:
        names = z.namelist()
        info = next(n for n in names if n.count("/") == 2 and n.startswith("Payload/") and n.endswith(".app/Info.plist"))
        bundle = plistlib.loads(z.read(info))["CFBundleIdentifier"]
        emb = info.replace("Info.plist", "embedded.mobileprovision")
        if emb not in names:
            return bundle, None
        with tempfile.NamedTemporaryFile(suffix=".mobileprovision") as t:
            t.write(z.read(emb))
            t.flush()
            prof = read_profile(Path(t.name))
    devices = list(prof.get("ProvisionedDevices", []))
    if prof.get("ProvisionsAllDevices"):
        devices.append("*")
    return bundle, devices


# --------------------------------------------------------------------------- plan

@dataclass
class Group:
    dest: str                      # path inside the container
    src: Path                      # local file or directory
    is_dir: bool
    files: dict[str, int]          # container-relative path -> size
    missing: list[str] = field(default_factory=list)
    mismatched: list[str] = field(default_factory=list)
    has_junk: bool = False

    @property
    def size(self) -> int:
        return sum(self.files.values())


def scan_tree(container: Path, rel: str) -> tuple[dict[str, int], bool]:
    files, junk = {}, False
    root = container / rel
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            if is_junk(name):
                junk = True
                continue
            p = Path(dirpath) / name
            files[p.relative_to(container).as_posix()] = p.stat().st_size
    return files, junk


def build_groups(container: Path, include_progress: bool) -> list[Group]:
    groups: list[Group] = []
    for rel in REQUIRED_TREES:
        root = container / rel
        if not root.is_dir():
            raise SetupError(f"备份里缺少 {rel}（--backup 应指向包含 container/ 的备份目录）")
        # One group per first-level subdirectory keeps each devicectl call bounded.
        for child in sorted(root.iterdir()):
            if is_junk(child.name):
                continue
            crel = f"{rel}/{child.name}"
            if child.is_dir():
                files, junk = scan_tree(container, crel)
                if files:
                    groups.append(Group(crel, child, True, files, has_junk=junk))
            else:
                groups.append(Group(crel, child, False, {crel: child.stat().st_size}))
    singles = list(REQUIRED_FILES) + [f for f in OPTIONAL_FILES if (container / f).is_file()]
    if include_progress:
        singles += [f for f in PROGRESS_FILES if (container / f).is_file()]
    for rel in singles:
        p = container / rel
        if not p.is_file():
            raise SetupError(f"备份里缺少 {rel}")
        groups.append(Group(rel, p, False, {rel: p.stat().st_size}))
    return groups


def compare(groups: list[Group], remote: dict[str, int]) -> None:
    """Missing files, plus cache files whose size differs (hash-named, so a size
    difference means an interrupted push), are re-pushed; other differing files
    (master data, keys, progress) are reported as mismatched."""
    for g in groups:
        g.missing, g.mismatched = [], []
        for f, size in g.files.items():
            if f not in remote:
                g.missing.append(f)
            elif remote[f] != size:
                (g.missing if f.startswith(CACHE_PREFIXES) else g.mismatched).append(f)


@dataclass
class Status:
    phase: str = "准备"
    device: str = ""
    total: int = 0
    done: int = 0
    files_total: int = 0
    files_done: int = 0
    current: str = ""
    started: float = field(default_factory=time.time)
    log: list[str] = field(default_factory=list)

    def note(self, msg: str) -> None:
        self.log.append(f"{time.strftime('%H:%M:%S')} {msg}")
        log(msg)


def serve_status(status: Status, port: int) -> None:
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):  # noqa: N802 - quiet
            pass

        def do_GET(self):  # noqa: N802
            pct = 100 * status.done / status.total if status.total else 0
            el = time.time() - status.started
            if self.path.startswith("/status"):
                body = json.dumps({**status.__dict__, "percent": pct, "elapsed": el}, ensure_ascii=False).encode()
                ctype = "application/json"
            else:
                body = f"""<!doctype html><meta charset=utf-8><meta http-equiv=refresh content=3>
<title>Linkura 设备安装</title><body style="font:15px -apple-system;margin:24px;max-width:720px">
<h2>{html.escape(status.device)}</h2><p>阶段：<b>{html.escape(status.phase)}</b></p>
<progress value="{pct:.1f}" max=100 style="width:100%;height:20px"></progress>
<p>{pct:.1f}%　{gb(status.done)} / {gb(status.total)}　文件 {status.files_done} / {status.files_total}　已用 {el/60:.1f} 分钟</p>
<p>当前：{html.escape(status.current)}</p><pre>{html.escape(chr(10).join(status.log[-15:]))}</pre>""".encode()
                ctype = "text/html; charset=utf-8"
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    log(f"进度页面：http://127.0.0.1:{port}/")


# --------------------------------------------------------------------------- main flow

def resolve_ipa(args, dev: Device, backup: Path, work: Path) -> Path | None:
    if args.skip_install:
        return None
    if args.ipa:
        ipa = Path(args.ipa)
        _, devices = ipa_info(ipa)
        if devices is None:
            raise SetupError(f"{ipa.name} 没有签名，请改用 --unsigned")
        if dev.udid not in devices and "*" not in devices:
            raise SetupError(f"{ipa.name} 的描述文件不包含 {dev.name}；改用 --unsigned 让脚本用最新描述文件重签")
        return ipa
    unsigned = Path(args.unsigned) if args.unsigned else None
    if unsigned is None:
        found = [p for p in sorted((backup / "client").glob("*-unsigned.ipa")) if not is_junk(p.name)] \
            if (backup / "client").is_dir() else []
        if len(found) != 1:
            raise SetupError("请用 --unsigned 指定未签名 IPA（或 --ipa 指定已签名 IPA / --skip-install 跳过安装）")
        unsigned = found[0]
    bundle, devices = ipa_info(unsigned)
    if devices is not None:
        log(f"  注意：{unsigned.name} 已带签名，将用新描述文件重签")
    profile = Path(args.profile) if args.profile else find_profile(bundle, dev.udid)
    identity = args.identity or find_identity()
    log(f"  签名身份：{identity}")
    if args.dry_run:
        return unsigned
    signed = work / unsigned.name.replace("-unsigned", "") .replace(".ipa", "-signed.ipa")
    subprocess.run(["bash", str(SIGNER), str(unsigned), str(profile), identity, str(signed)], check=True)
    return signed


def wait_for_server(dev: Device, bundle: str, before: int, work: Path, seconds: int = 90) -> bool:
    deadline = time.time() + seconds
    local = work / "server.log"
    while time.time() < deadline:
        time.sleep(5)
        try:
            if local.exists():
                local.unlink()
            pull(dev, bundle, "Documents/LinkuraOffline/server.log", local)
        except SetupError:
            continue
        data = local.read_bytes()
        tail = data[before:] if len(data) >= before else data
        if b"listening on" in tail:
            return True
        if b"not started" in tail or b"bind failed" in tail:
            log(tail.decode(errors="replace")[-600:])
            return False
    return False


def run(args) -> int:
    backup = Path(args.backup).expanduser().resolve()
    container = backup / "container" if (backup / "container").is_dir() else backup
    status = Status()
    if args.port:
        serve_status(status, args.port)

    log("== 1/6 设备")
    dev = pick_device(args.device)
    status.device = f"{dev.name}（{dev.model}）"
    cap = f"，容量 {dev.capacity / 1e9:.0f} GB" if dev.capacity else ""
    log(f"  {dev.name}  {dev.model}  iOS {dev.os_version}{cap}  {dev.udid}")

    with tempfile.TemporaryDirectory(prefix="linkura-setup-") as tmp:
        work = Path(tmp)
        log("== 2/6 客户端")
        status.phase = "准备客户端"
        ipa = resolve_ipa(args, dev, backup, work)
        bundle = ipa_info(ipa)[0] if ipa else args.bundle_id
        app = installed_app(dev, bundle)
        log(f"  Bundle ID：{bundle}；设备上{'已安装 ' + app.get('version', '') if app else '尚未安装'}")
        if ipa:
            log(f"  安装包：{ipa}")

        log("== 3/6 对比数据")
        status.phase = "对比数据"
        groups = build_groups(container, args.include_progress)
        remote: dict[str, int] = {}
        if app:
            status.current = "读取设备文件列表（文件多时需要一两分钟）"
            remote.update(device_files(dev, bundle, "Documents"))
            if args.include_progress:
                remote.update(device_files(dev, bundle, "Library/Application Support/LinkuraOffline"))
        compare(groups, remote)
        progress_exists = args.include_progress and PROGRESS_DB in remote
        for g in groups:
            if g.dest.startswith("Library/") and progress_exists:
                # Never merge two progress databases file-by-file.
                g.missing = list(g.files) if args.overwrite_progress else []
                g.mismatched = []
        all_files = sum(len(g.files) for g in groups)
        total = sum(g.size for g in groups)
        mism = [f for g in groups for f in g.mismatched]
        todo = [g for g in groups if g.missing or (args.overwrite and g.mismatched)]
        push_files = sum(len(g.missing) + (len(g.mismatched) if args.overwrite else 0) for g in todo)
        need = sum(g.files[f] for g in todo for f in g.missing + (g.mismatched if args.overwrite else []))
        same = sum(1 for g in groups for f in g.files if remote.get(f) == g.files[f])
        log(f"  备份：{all_files} 个文件，{gb(total)}；设备上已有且一致：{same} 个")
        log(f"  需要推送：{push_files} 个文件，{gb(need)}")
        if mism:
            log(f"  大小不一致：{len(mism)} 个（{'将覆盖' if args.overwrite else '保留设备版本，--overwrite 可覆盖'}）")
            for f in mism[:5]:
                log(f"    {f}")
        if progress_exists:
            log("  设备上已有玩家进度库：" + ("将覆盖，旧版本先拉回本地保存" if args.overwrite_progress
                                      else "不覆盖（需要时加 --overwrite-progress）"))

        if args.dry_run or args.verify_only:
            ok = not any(g.missing for g in groups)
            log(f"== {'演练结束，未改动设备' if args.dry_run else '校验结果'}：" +
                ("设备数据完整" if ok else f"还缺 {sum(len(g.missing) for g in groups)} 个文件"))
            return 0 if (ok or args.dry_run) else 1

        log("== 4/6 安装")
        if ipa or todo:
            terminate_app(dev, app)
        if ipa:
            status.phase = "安装客户端"
            first_install = app is None
            devicectl(["device", "install", "app", "--device", dev.udid, str(ipa)], timeout=900)
            log("  已安装（覆盖安装会保留设备上的数据）")
            if first_install:
                log("  首次安装：如果这台设备还没信任过这个签名身份，客户端会打不开。"
                    "趁推送期间在设备上打开 " + TRUST_STEPS + "。")
            app = installed_app(dev, bundle)
        if not app:
            raise SetupError("设备上没有该客户端，无法推送数据（去掉 --skip-install 或指定 IPA）")

        log("== 5/6 推送数据")
        if progress_exists and args.overwrite_progress:
            keep = Path(args.progress_backup_dir or f"linkura-progress-{dev.udid}-{time.strftime('%Y%m%d-%H%M%S')}")
            keep.mkdir(parents=True, exist_ok=False)
            for rel in PROGRESS_FILES:
                if rel in remote:
                    pull(dev, bundle, rel, keep / Path(rel).name)
            log(f"  设备上的旧进度已保存到 {keep.resolve()}")
        status.phase, status.total, status.files_total = "推送数据", need, push_files
        for i, g in enumerate(todo, 1):
            files = g.missing + (g.mismatched if args.overwrite else [])
            status.current = f"[{i}/{len(todo)}] {g.dest}"
            whole = g.is_dir and not g.has_junk and (args.overwrite or not g.mismatched)
            try:
                if whole:
                    push(dev, bundle, g.src, g.dest)
                else:
                    for f in files:
                        push(dev, bundle, container / f, f)
            except DeviceFull:
                status.phase = "设备空间不足"
                raise SetupError(
                    f"{dev.name} 存储空间已满，还有约 {gb(status.total - status.done)} 没推送。"
                    "在 设置 → 通用 → iPhone/iPad 储存空间 里清理（建议再多留 1 GB），"
                    "然后运行同一条命令继续") from None
            status.done += sum(g.files[f] for f in files)
            status.files_done += len(files)
            if i % 25 == 0 or i == len(todo):
                el = time.time() - status.started
                status.note(f"  {i}/{len(todo)} 组  {gb(status.done)} / {gb(status.total)}  {el / 60:.1f} 分钟")

        log("== 6/6 校验")
        status.phase = "校验"
        remote = device_files(dev, bundle, "Documents")
        if args.include_progress:
            remote.update(device_files(dev, bundle, "Library/Application Support/LinkuraOffline"))
        compare(groups, remote)
        left = [f for g in groups for f in g.missing]
        if left:
            status.phase = "校验失败"
            raise SetupError(f"仍缺 {len(left)} 个文件，例如 {left[:3]}；可以直接重新运行脚本补推")
        status.note(f"  设备数据完整：{all_files} 个文件，{gb(total)}")

        if args.launch:
            status.phase = "启动"
            before = remote.get("Documents/LinkuraOffline/server.log", 0)
            launch_app(dev, bundle)
            ok = wait_for_server(dev, bundle, before, work)
            status.note("  离线服务已启动（server.log: listening）" if ok else
                        "  90 秒内没看到离线服务启动记录：请在设备上看是否停在标题页，并检查 server.log")
            if not ok:
                return 1
        status.phase = "完成"
        log("完成。")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="把 Linkura 离线客户端和备份数据装到 iOS 设备上")
    ap.add_argument("--backup", default=str(REPO) if (REPO / "container").is_dir() else None,
                    help="备份目录（包含 container/）；脚本放在备份盘的 tools/ 里运行时可省略")
    ap.add_argument("--device", help="设备名称或 UDID；省略时使用唯一已连接的设备")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--unsigned", help="未签名 IPA，脚本自动找描述文件和签名身份")
    src.add_argument("--ipa", help="已签名 IPA（描述文件必须包含该设备）")
    src.add_argument("--skip-install", action="store_true", help="不安装，只推送/校验数据")
    ap.add_argument("--profile", help="指定描述文件（默认自动选择最新的匹配项）")
    ap.add_argument("--identity", help="指定签名身份（默认唯一的 Apple Development 身份）")
    ap.add_argument("--bundle-id", default=DEFAULT_BUNDLE, help=f"--skip-install 时使用（默认 {DEFAULT_BUNDLE}）")
    ap.add_argument("--include-progress", action="store_true", help="同时推送备份里的玩家进度库")
    ap.add_argument("--overwrite-progress", action="store_true", help="覆盖设备上已有的进度库（先拉回本地保存）")
    ap.add_argument("--progress-backup-dir", help="--overwrite-progress 时保存旧进度的目录")
    ap.add_argument("--overwrite", action="store_true", help="覆盖设备上大小不一致的文件")
    ap.add_argument("--launch", action="store_true", help="完成后启动客户端并确认离线服务已启动")
    ap.add_argument("--dry-run", action="store_true", help="只检查和列出计划，不改动设备")
    ap.add_argument("--verify-only", action="store_true", help="只校验设备数据是否完整")
    ap.add_argument("--port", type=int, help="在 127.0.0.1:<port> 显示进度页面")
    args = ap.parse_args(argv)
    if not args.backup:
        ap.error("请用 --backup 指定备份目录")
    if args.overwrite_progress and not args.include_progress:
        ap.error("--overwrite-progress 需要和 --include-progress 一起使用")
    if args.verify_only:
        args.skip_install = True
    if shutil.which("xcrun") is None:
        log("需要安装 Xcode（xcrun devicectl）")
        return 2
    try:
        return run(args)
    except SetupError as e:
        log(f"错误：{e}")
        return 1
    except KeyboardInterrupt:
        log("已中断；重新运行会跳过已经推送的文件")
        return 130


if __name__ == "__main__":
    sys.exit(main())
