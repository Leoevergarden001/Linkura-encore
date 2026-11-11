# linkura-offline

**非官方**的「Link！Like！ラブライブ！蓮ノ空スクールアイドルクラブ」（リンクラ）离线运行项目：在官方服务器于 2026-06-30 停止服务之后，让你自己手上的客户端和资源缓存继续离线游玩，并方便在此基础上继续开发和修复。

An unofficial offline server and client patch set for Link! Like! LoveLive! (リンクラ). Bring your own client and asset cache; nothing here contains game files.

> 本项目与官方无关，不提供、不分发任何游戏安装包、资源或账号数据。请只用于你自己合法持有的客户端和数据。

## 能做什么

- **离线服务器**（`server/`）：在设备本机 `127.0.0.1` 上回应游戏的全部 API，玩家进度保存在本地 SQLite。Android 和 iOS 共用同一份代码。
- **Android 5.1.0**（`android/`）：LSPatch 模块，接管 API 请求、修复离线时的场景切换等问题，并禁用会清掉资源的危险操作。
- **iOS / iPadOS 5.0.1**（`ios/` + `tools/`）：不越狱，把离线服务器作为 dylib 打进 IPA，并对客户端做按字节校验的静态补丁；自签名后安装。
- **工具**（`tools/`）：打包、签名、一键安装（iOS）、资源包制作和导入（Android）、资源缓存覆盖率分析、逆向辅助。

登录、主页、SHOW（リズムゲーム）、Dream Live、扭蛋、卡牌养成、编成、任务、个人资料、收藏等主要功能可以离线使用。没保存下来的内容（剧情配音、歌词 MV、存档视频等）见 [已知限制](docs/known-limitations.md)。

## 需要准备

| | Android | iOS / iPadOS |
|---|---|---|
| 客户端 | 你自己的 5.1.0 APK | 你自己的、已解密的 5.0.1 IPA |
| 资源 | 你设备上游戏的资源缓存（`files/D`、`files/M`）和存储密钥 | 同左（Android 缓存可转用） |
| 环境 | Android SDK/NDK、JDK 17、Java 21+（LSPatch） | Xcode、Apple 账号（免费 Personal Team 即可） |

## 快速开始

```sh
git clone --recurse-submodules <this repo>
```

- [入门与版本说明](docs/getting-started.md)
- [Android 构建与安装](docs/android.md) ・ [iOS 构建与安装](docs/ios.md) ・ [准备资源](docs/resources.md)

## 开发

- [整体架构](docs/architecture.md)
- [离线服务器：接口、数据库、数据生成](docs/server.md)
- [iOS 补丁怎么找、怎么加](docs/patching-ios.md) ・ [Android Hook](docs/hooking-android.md)
- [回归检查清单](docs/regression-checklist.md)

```sh
server/build-host.sh && python3 server/tests/smoke_test.py   # 在电脑上跑离线服务器的冒烟测试
python3 -m unittest discover -s tools/tests -t .                # 工具测试
python3 -m unittest discover -s server/generators/tests         # 数据生成测试（需要 PyYAML）
```

## 许可

GPL-3.0（见 [LICENSE](LICENSE)）。本项目派生自 [ChocoLZS/linkura-localify](https://github.com/ChocoLZS/linkura-localify)，保留其提交历史；来源与第三方组件见 [NOTICE.md](NOTICE.md)。
