# iOS 补丁：怎么找，怎么加

iOS 上不能运行时 Hook，客户端行为只能在打包时静态修改 UnityFramework。补丁表在 `tools/linkura_offline/ios_offline.py`，每一项是：

```python
(地址, "说明", "原始字节（小端 hex）", "新字节")
```

`patch_code` 先核对原始字节，不一致就报错停止，所以补丁只会作用在验证过的 5.0.1 构建上，不会把别的版本改坏。

## 准备

```sh
# 符号表：地址 -> 方法名（来自你的 IPA 的 metadata 和 UnityFramework）
python3 -m tools.linkura_offline ios-symbols --ipa <IPA> --output sym.json
unzip -j <IPA> 'Payload/*.app/Frameworks/UnityFramework.framework/UnityFramework' -d .
alias re='python3 tools/re/il2cpp_re.py -f UnityFramework -s sym.json'
```

## 找补丁位置（以 Chapter RANK 为例）

1. **按名字找入口**：`re find 'ChapterPoint::OnPointerClick'`。async 方法的逻辑在编译器生成的状态机里：`re find '<OnPointerClick>d__6::MoveNext'`。
2. **反汇编**：`re dis <地址> 0x600`，每个调用都标出了目标方法名，比如 `-> Tecotec.CommonApiCache::get_CurrentChapterRank`，后面紧跟 `cbz w0, <提示分支>`。
3. **看谁还在用**：`re callers 'CommonApiCache::get_CurrentChapterRank$'`。确认改动范围：这个 getter 还被顶栏面板调用，所以改调用点的分支，而不是改 getter 本身。
4. **读原始字节**：`re bytes 0x2eb0fe8`，得到 `00070034`（`cbz w0, +0xe0`）。
5. **写新指令**：无条件 `b +0xe0` 是 `0x14000000 | (0xe0 >> 2)` = `0x14000038`，小端 `38000014`。
6. **加进补丁表**（单独一组，配一个函数和 `stage_offline_ios_client` 的开关、CLI 的 `--allow-*` 参数），并在 `tools/tests/test_ios_offline.py` 加测试：校验原指令、新指令的类型和跳转目标。
7. 打包、安装、在设备上验证；`server.log` 用来确认不再出现原来的请求。

## 常用指令编码（AArch64）

| 目的 | 写法 |
|---|---|
| 函数直接返回（void） | `ret` = `c0035fd6` |
| 空操作 | `nop` = `1f2003d5` |
| 无条件跳转 | `b off` = `0x14000000 \| ((off >> 2) & 0x3FFFFFF)` |
| 读条件分支目标 | `cbz/cbnz`：`imm19 = (word >> 5) & 0x7FFFF`，目标 = 地址 + `imm19 << 2` |

## 已有的补丁组

| 组 | 作用 | 关闭 |
|---|---|---|
| `COMPAT_PATCHES` | 5.1 资源与 5.0.1 客户端的兼容（Dream Live 成员面板查找方式） | 不可关闭 |
| `LYRIC_VIDEO_PATCHES` | SHOW 歌词 MV 视为关闭（MV 资源没有保存） | 默认关，`--disable-lyric-videos` 打开 |
| `DESTRUCTIVE_ACTION_PATCHES` | 缓存删除、登出、删除用户等 8 个入口直接返回 | `--allow-destructive-actions` |
| `STORY_PLAYBACK_PATCHES` | 剧情再生确认的 OK 改为关闭 | `--allow-story-playback` |
| `ARCHIVE_PLAYBACK_PATCHES` | スクールアイドルコネクト ARCHIVES 里点存档不再进入回放（回放需要的 3D 资源没有保存，会弹下载错误） | `--allow-archive-playback` |
| `TITLE_ID_PATCHES` | 标题页不显示玩家 ID | — |
| `CHAPTER_PORTAL_PATCHES` | Chapter RANK 点击改为期间外提示 | `--allow-chapter-portal` |
| `RES_VERSION_PATCHES` | 资源版本始终报告缓存的版本（`--res-version`），不读每个玩家的本地存储 | `--per-player-res-version` |
| `CATALOG_SYNC_PATCHES` | 保留缓存里的资源目录，不下载资源清单 | `--allow-catalog-sync` |
| `PLAYER_ID_PATCHES` | 玩家 ID 固定为 000000000（和 Android 一致），不需要原账号的 PlayerPrefs | `--keep-player-id` |
| `DEVICE_ID_PATCHES` | 设备专属 ID 也返回 000000000。原本为空，打开ヘルプ等网页时会闪退 | `--keep-player-id` |

后三组让 iOS 只凭资源目录库的存储密钥就能运行。需要返回一个字符串时，借用一个运行时不会用到的字符串常量（Mono BTLS 的报错信息），改写成需要的值，再把方法的元数据初始化指向它的槽位。

metadata 字符串（API 地址、BGM 名等）的改写也在同一个文件里（`rewrite_literals`、`rewrite_default_values`），原理是原地缩短字符串，不移动任何偏移。
