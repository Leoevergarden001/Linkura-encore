[English](README.md) | **简体中文** | [日本語](README.ja.md)

# Linkura-encore

<sub>原名 linkura-offline。代码、工具和设备上的目录仍沿用这个名字。</sub>

**非官方**的「Link！Like！ラブライブ！蓮ノ空スクールアイドルクラブ」（リンクラ）离线运行项目：在官方服务器停止服务之后，让你自己手上的客户端和资源缓存继续离线游玩，并方便在此基础上继续开发和修复。

> 本项目与官方无关，旨在保存已停止服务的游戏体验，并供个人研究与技术交流。本项目与游戏的开发商、发行商及其他权利人无关，也未获得其认可或授权。
>
> 项目维护者不提供收费安装、资源销售或官方技术支持。仓库代码依照 [GPL-3.0](LICENSE) 授权；项目的保存与交流目的不改变该许可证赋予的权利和义务。
>
> 本软件按现状提供，不保证功能完整、稳定或适用于特定设备。使用前请自行备份数据。在适用法律允许的范围内，作者及贡献者不提供担保，也不对使用或无法使用本软件造成的损失承担责任；具体以 GPL-3.0 的条款为准。

## 功能概览

本项目让原版客户端在设备上连接本地离线服务，并读取你已经保存的游戏资源。标题页的 LOGIN 会进入本地游戏状态；它不连接官方服务器，也不会恢复原账号。Android 和 iOS 使用同一套离线服务器及玩家状态逻辑。

- **主页与日常操作**：进入 Home，查看卡牌、任务、个人资料与收藏，使用已恢复的菜单和商店入口。部分入口只保留空状态或安全提示，见下方的已知限制。
- **SHOW 与スクステ**：进入 SHOW（リズムゲーム）并完成结算；游玩スクールアイドルステージ（スクステ）的 Daily Live、Quest Live、Learning Live、Grade Live、Dream Live、Live Grand Prix 等模式。可显示的歌曲、画面和演出取决于你的资源缓存。
- **扭蛋与卡牌**：使用已接入的本地卡池抽卡，查看抽卡履历与点数；进行卡牌育成、编成和道具兑换。这里的卡池和概率不代表官方服务的完整历史配置。
- **本地进度**：抽卡结果、持有卡牌、编队和关卡结算等状态写入设备上的 SQLite 数据库，重启后继续使用。离线玩家 ID 固定为 `000000000`，不是原官方账号。
- **资源整理与迁移**：工具可以检查缓存覆盖率、制作并导入 Android 资源包，以及将适用的 Android 缓存转换供 iOS 使用。转换不能补回原本没有保存的内容。

这些功能需要与你的客户端版本匹配的缓存；部分内容仍受资源缺失影响。

## 支持的客户端

| 平台 | 当前支持版本 | 安装方式 |
|---|---|---|
| Android | 5.1.0 | LSPatch 将离线模块封装进 APK；目标设备不需要 root 或单独安装模块管理器 |
| iOS / iPadOS | 5.0.1 | 将离线组件和补丁写入 IPA，再用你自己的 Apple 签名安装；目标设备不需要越狱 |

目前只针对上述客户端构建和验证。iOS 的静态补丁会校验原始字节，因此不能直接用于其他版本或不同构建。两端都需要匹配的资源版本；详见[版本说明](docs/zh/getting-started.md)。

## 使用前要准备什么

本仓库提供代码、构建工具和离线服务数据，不包含原版游戏 APK、IPA、完整资源缓存或账号资料。仅克隆仓库不能得到可游玩的客户端。

1. **客户端**：Android 5.1.0 APK，或已解密的 iOS 5.0.1 IPA。普通 App Store IPA 不能直接用于当前的 iOS 补丁流程。
2. **资源**：你设备上保存的 `D/` 资源缓存、`M/` 中的资源目录库与主数据库，以及和这两个数据库配对的存储密钥。只有资源文件而没有数据库或密钥，不能完成新设备安装。参见[准备资源](docs/zh/resources.md)。
3. **电脑与设备**：Android 安装需要 ADB；iOS 安装需要 Mac、Xcode 工具、已信任且开启开发者模式的设备，以及覆盖该设备的签名描述文件。自行构建时还需要相应 SDK、编译器和 Python 依赖，详见[入门指南](docs/zh/getting-started.md)。

资源缓存可能占用十几 GB 或更多空间；制作资源包和向设备导入时，还需要为中间文件留出空间。资源包和设备备份可能包含私有数据；请保留原始归档，并在改动设备前备份现有进度。打包工具默认会为资源目录库和主数据库生成新密钥，不带原账号的 `app` 数据库。

## 选择安装路径

以下两条路径都需要本仓库的安装工具。如果还没有获取代码，先运行：

```sh
git clone --recurse-submodules https://github.com/Leoevergarden001/Linkura-encore.git
cd Linkura-encore
```

### 已有离线安装包与资源包

- **Android**：准备与当前版本匹配的离线 APK 和 Android 资源包。先核对文件的 SHA-256。若资源包是 `.tar.zst`，先检查归档并解压到新目录；安装工具的 `--pack` 参数接收包含 `files/D/`、`files/M/` 和 `files/linkura-local/storage-key.json` 的**目录**，不能直接接收压缩文件。已有官方版或其他签名的客户端时，先备份数据；不能直接覆盖不同签名的 APK。参见[Android 安装](docs/zh/android.md)。
- **iOS / iPadOS**：准备未签名的离线 IPA 和包含 `container/` 的资源目录，按[iOS 安装指南](docs/zh/ios.md)用自己的签名安装并传输资源。若已有适用于该设备的已签名 IPA，安装脚本改用 `--ipa`（参数见 `python3 tools/ios_device_setup.py --help`）。免费开发签名通常需要定期续签。

现成安装包省去了对应客户端的编译步骤，但不会省去版本核对、资源导入；未签名 iOS IPA 仍需签名。资源包也不能代替 APK 或 IPA。

### 从自己的客户端和缓存构建

按[入门与版本说明](docs/zh/getting-started.md)检查输入，再选择[Android 构建与安装](docs/zh/android.md)或[iOS 构建与安装](docs/zh/ios.md)。资源缓存的提取、换密钥和 Android→iOS 转换步骤在[准备资源](docs/zh/resources.md)。仓库还提供[供 Agent 使用的安装 Skill](.agents/skills/linkura-offline-setup/SKILL.md)；它会根据已有输入选择对应流程，但仍需要你提供客户端、资源和设备。

## 安装后如何确认

1. 让设备离线或开启飞行模式，冷启动游戏，在标题页点 LOGIN，确认进入 Home。
2. 打开一个已缓存的 SHOW 歌曲或スクステ关卡，确认能进入、结算并返回。
3. 执行一项会改变本地状态的操作，重启后确认结果仍在；不要用删除缓存、登出或删除用户来测试。
4. 如果出现下载错误、空白画面或无法进入的页面，先对照下方的已知限制，再查看[回归检查清单](docs/zh/regression-checklist.md)及平台日志。

通过这些检查仅说明所测流程和所用缓存可运行，不能证明所有歌曲、关卡和资源都完整。

## 已知限制

离线版不能从已关闭的官方 CDN 补下载资源。未保存的剧情配音、歌词 MV、活动存档视频等内容无法播放；部分公告、社交和 Chapter 页面只有空状态或提示。为了保护不可重新下载的缓存，キャッシュ削除、ログアウト、ユーザー削除等操作已被拦截。完整范围与原因见[已知限制](docs/zh/known-limitations.md)。

## 项目结构与开发

| 目录 | 用途 |
|---|---|
| `server/` | Android 与 iOS 共用的离线 API、响应模板和本地玩家状态 |
| `android/` | Android Hook、LSPatch 模块与客户端侧离线修复 |
| `ios/` | iOS 离线组件；客户端静态补丁和封装工具位于 `tools/` |
| `tools/` | 客户端封装、设备安装、资源处理和分析工具 |

想了解调用流程或继续开发，可从[整体架构](docs/zh/architecture.md)、[离线服务器](docs/zh/server.md)、[Android Hook](docs/zh/hooking-android.md)和[iOS 补丁](docs/zh/patching-ios.md)开始。iOS 资源也可以放在自己的服务器上由 App 下载，见[远程资源服务器](docs/zh/remote-assets.md)。修改后按[回归检查清单](docs/zh/regression-checklist.md)验证；术语对照见[术语表](docs/glossary.md)。

```sh
server/build-host.sh && python3 server/tests/smoke_test.py
python3 -m unittest discover -s tools/tests -t .
python3 -m unittest discover -s server/generators/tests
```

最后一项需要 PyYAML。上述命令检查主机版离线服务器、工具和数据生成器，不能替代真机或模拟器回归。

## 许可与来源

仓库代码按 GPL-3.0 授权，见 [LICENSE](LICENSE)。本项目派生自 [ChocoLZS/linkura-localify](https://github.com/ChocoLZS/linkura-localify)，保留其提交历史。来源、第三方组件和游戏内容权利说明见 [NOTICE.md](NOTICE.md)。
