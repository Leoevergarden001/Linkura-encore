# 离线服务器开发

## 在电脑上运行

```sh
server/build-host.sh                     # -> server/build/host/linkura-offline-server
python3 server/tests/smoke_test.py       # 在临时目录启动服务器，逐个检查主要接口
```

冒烟测试是改服务器后最快的检查，也是接口行为的活文档：每个 `check(...)` 对应一条游戏里验证过的行为。

主机版也能测试 iOS 的远程资源代理（`--remote-asset-base`、`--remote-asset-key` 等），见 [远程资源服务器](remote-assets.md#在电脑上测试)。

## 新增或修改一个接口

1. 在设备日志里找到未匹配的路径（Android `no mock file or registered route`，iOS `no route for`），以及客户端期望的响应结构。响应 DTO 的字段名可以在 IL2CPP 的 `Org.OpenAPITools.Model.*` 类里找到（`tools/re/il2cpp_re.py find`）。
2. 静态响应：把 JSON 放进 `server/data/builtin/`，在 `server/src/RouteRegistry.cpp` 用 `RegisterStaticJson` 注册。
3. 有状态的响应：在 `HttpMockBackend` 里实现（读写 SQLite），用 `RegisterBackend` 注册。需要新表时加 `server/db/schema/NNN_*.sql`；已有玩家库的升级写在 `server/db/player-state/`。
4. 枚举值不要猜：从 metadata 的字段默认值读（见 [iOS 补丁](patching-ios.md) 的工具），或参考已有代码中的注释。
5. 在 `server/tests/smoke_test.py` 加一条检查，跑通后再上设备。
6. 不重新打包也能试验：Android 和 iOS 都支持按路径放置覆盖文件（见各平台文档的"调试"）。

## 数据库

玩家库是一个 SQLite 文件（Android：游戏的 `files/HasuKikaisann.sqlite3`；iOS：`Library/Application Support/LinkuraOffline/`）。首次启动时按 `schema/` 建表、导入 `seed/`，得到"博物馆存档"：完整收藏、养成到满。玩家之后的操作（编成、养成、扭蛋、任务、SHOW 成绩）写在同一个库里。

## 内置数据的生成

`server/data/builtin` 和 `server/db/seed` 大部分由脚本从主数据生成：

```sh
pip install pyyaml
git submodule update --init third_party/link-like-diff
python3 server/generators/generate_all.py
# 主数据在别处：LINKURA_MASTER_DIR=<path> python3 server/generators/generate_all.py
python3 -m unittest discover -s server/generators/tests
```

- 路径统一在 `server/generators/paths.py`。
- SQL 种子还包含从在线服务器抓取的存档详情（`server/generators/data/archive_archive_details.json`，不在仓库中）。缺少它时 `generate_all.py` 只重新生成 JSON。
- `user_card_get_list.json` 中有一张卡（角色 1053）的 `generations_id` 保留了抓取值 0，生成器会算成 100；以仓库中的值为准。
- 扭蛋池定义 `server/src/backend/MuseumGachaCatalog.hpp` 由 `generate_gacha_catalog.py` 生成，说明见 `server/GACHA_MUSEUM.md`。
- スクールアイドルコネクト：存档列表 `archive_get_archive_list.json`（398 个），With×STATION 的列表和详情 `archive_get_with_station_list.json`、`archive_with_station_details.json`（5 期），存档的官方视频地址 `archive_video_urls.json`（供 `motionReplays` 关闭时使用）。首页「Recent」显示最新一期 With×STATION。
- 存档详情由 `server/db/player-state/008_archive_replay_urls.sql`（动捕回放入口）和 `009_mrs_video_replays.sql`（5.0.1 客户端播不了 mrs 动捕，With×MEETS 的 mrs 存档改走视频）在每次启动时修正。
