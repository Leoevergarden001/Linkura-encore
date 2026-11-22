<!-- Translation source: 85ba8e531f12739a3a402a5389f6af013d337e19 (docs/zh/server.md) -->

# Developing the offline server

## Run on a computer

```sh
server/build-host.sh                     # -> server/build/host/linkura-offline-server
python3 server/tests/smoke_test.py       # start in a temporary directory and check the main APIs
```

Smoke tests are the fastest check after changing the server and serve as living documentation for route behavior: each `check(...)` corresponds to behavior verified in the game.

The host build can also test the iOS remote asset proxy (`--remote-asset-base`, `--remote-asset-key` and so on); see [remote asset host](remote-assets.md#testing-on-a-computer).

## Add or modify an API route

1. Find the unmatched path in device logs (`no mock file or registered route` on Android, `no route for` on iOS) and identify the expected response structure. Field names for response DTOs are in IL2CPP `Org.OpenAPITools.Model.*` classes (`tools/re/il2cpp_re.py find`).
2. For a static response, add JSON under `server/data/builtin/` and register it with `RegisterStaticJson` in `server/src/RouteRegistry.cpp`.
3. For a stateful response, implement it in `HttpMockBackend` using SQLite and register it with `RegisterBackend`. Add a table under `server/db/schema/NNN_*.sql` when needed; put upgrades for existing player databases in `server/db/player-state/`.
4. Do not guess enum values. Read default field values from metadata with the tools in [iOS patching](patching-ios.md), or consult comments in existing code.
5. Add a check to `server/tests/smoke_test.py`. After it passes, verify on a device.
6. To experiment without repacking, both Android and iOS accept path-based override files; see the debugging sections of their platform guides.

## Database

The player database is a single SQLite file: `files/HasuKikaisann.sqlite3` in the Android app or `Library/Application Support/LinkuraOffline/` on iOS. On first launch, the server creates tables from `schema/` and loads `seed/` to create a **museum save** with a full collection and maximum development. Later actions (編成, development, gacha, missions, SHOW scores) are saved in the same database.

## Generate embedded data

Most of `server/data/builtin` and `server/db/seed` is generated from master data:

```sh
pip install pyyaml
git submodule update --init third_party/link-like-diff
python3 server/generators/generate_all.py
# For master data elsewhere: LINKURA_MASTER_DIR=<path> python3 server/generators/generate_all.py
python3 -m unittest discover -s server/generators/tests
```

- `server/generators/paths.py` centralizes paths.
- SQL seeds also include archived details captured from the online server (`server/generators/data/archive_archive_details.json`, not in this repository). Without that file, `generate_all.py` regenerates only JSON.
- In `user_card_get_list.json`, one card (character 1053) retains the captured `generations_id` value 0; the generator would calculate 100. Use the checked-in value.
- `generate_gacha_catalog.py` generates gacha-pool definitions in `server/src/backend/MuseumGachaCatalog.hpp`; see `server/GACHA_MUSEUM.md`.
- スクールアイドルコネクト: the archive list `archive_get_archive_list.json` (398 archives), the With×STATION list and details `archive_get_with_station_list.json` and `archive_with_station_details.json` (5 episodes), and the archives' official video URLs `archive_video_urls.json` (used with `motionReplays` off). The home "Recent" row shows the latest With×STATION episode.
- `server/db/player-state/008_archive_replay_urls.sql` (motion replay entries) and `009_mrs_video_replays.sql` (the 5.0.1 client cannot play mrs motion data, so With×MEETS mrs archives play video) correct the archive details on every start.
