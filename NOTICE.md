# NOTICE

Linkura-encore (formerly linkura-offline) is licensed under the GNU General Public License v3.0 (see `LICENSE`).

## Origin

This repository is derived from **[ChocoLZS/linkura-localify](https://github.com/ChocoLZS/linkura-localify)** (GPL-3.0),
and keeps its history up to commit `38dfc9a` ("bump version", 2026-07-02). Later commits add the offline
server, the iOS runtime and tools, restructure the tree and remove features the offline runtime does not use
(free camera, translation, render settings, story display switches, the settings app and overlays).
The Android module keeps the original application id and entry class names because its native symbols,
the LSPatch module id and the packaging tools depend on them.

linkura-localify acknowledges, and so do we:

- [gkmasToolkit](https://github.com/kishidanatsumi/gkmasToolkit)
- [UmaPyogin-Android](https://github.com/akemimadoka/UmaPyogin-Android)
- [UnityResolve.hpp](https://github.com/issuimo/UnityResolve.hpp)
- [Il2CppDumper](https://github.com/Perfare/Il2CppDumper)
- [gakuen-imas-localify](https://github.com/chinosk6/gakuen-imas-localify)
- [inspix-hailstorm](https://github.com/vertesan/inspix-hailstorm)

## Third-party components

| Component | Where | License |
|---|---|---|
| [UnityResolve.hpp](https://github.com/issuimo/UnityResolve.hpp) | `android/app/src/main/cpp/deps/UnityResolve` | MIT |
| [{fmt}](https://github.com/fmtlib/fmt) 11.0.2 | `android/app/src/main/cpp/deps/fmt-11.0.2` | MIT |
| [nlohmann/json](https://github.com/nlohmann/json) | `third_party/nlohmann` | MIT |
| [SQLite](https://sqlite.org) (amalgamation) | `third_party/sqlite` | Public domain |
| [LSPatch](https://github.com/LSPosed/LSPatch) 0.7 | `android/lspatch/lspatch.jar`; `lspatch_loaderfix.jar` is rebuilt from it by `android/lspatch/loaderfix/build.sh` (sources in that directory) | GPL-3.0 |
| [ShadowHook](https://github.com/bytedance/android-inline-hook), [xDL](https://github.com/hexhacking/xDL) | Gradle dependencies | MIT |
| [Xposed API](https://github.com/rovo89/XposedBridge) | Gradle dependency (compile only) | Apache-2.0 |
| [vertesan/link-like-diff](https://github.com/vertesan/link-like-diff) | `third_party/link-like-diff` (git submodule; game MasterData as YAML) | see that repository |

`tools/resources/hailstorm.py` reimplements the raw-asset key derivation of
[vertesan/inspix-hailstorm](https://github.com/vertesan/inspix-hailstorm) (`manifest/asset.go`, AGPL-3.0).

## Game data

The game, its client binaries, assets, MasterData and names belong to their respective rights holders.
This repository does not contain client binaries, asset caches or account data. `server/data` and `server/db`
hold response templates and seed data generated from MasterData (see `server/generators`) so that the
offline server can answer the client.
