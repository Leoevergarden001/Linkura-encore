# 准备资源

官方 CDN 已经关闭，资源只能来自你自己设备上游戏下载过的缓存。

## 缓存里有什么

游戏的外部文件目录（Android：`/sdcard/Android/data/com.oddno.lovelive/files`；iOS：App 容器的 `Documents/`）：

| 路径 | 内容 |
|---|---|
| `D/<两位前缀>/<RealName>` | 资源（Hailstorm 缓存）：UnityFS 资源包、音频（acb/awb）、视频（usm）、加密的文本等 |
| `M/` | 主数据库（SQLCipher） |

`M/` 里的数据库按**存储密钥**命名和加密。客户端为每个存储域在 PlayerPrefs（Android：`shared_prefs/com.oddno.lovelive.v2.playerprefs.xml`）里保存一个 256 字节的随机 seed：

| 存储域 | PlayerPrefs 名 | `M/` 里的数据库 | 离线版是否需要 |
|---|---|---|---|
| `//catalog` | `JWbAatNSI1NpHhCdU%2BgJllY0BTU%3D` | 资源目录（哪个资源对应 `D/` 里哪个文件） | 需要 |
| `//master` | `Dr%2BcmYmVx%2B1kQ8uTmoYADAduWVk%3D` | 主数据（约 57 MB） | 需要：客户端不会从缓存重新生成空的主数据库 |
| `app` | — | 玩家 ID、本地设置等 | 不需要，而且含原账号身份，不要带 |

- 数据库文件名 = 小写、无填充的 Base32(MD5(seed))。
- SQLCipher 4 原始密钥 = SHA-256(seed)（两个库都是）。
- `D/` 的资源用资源清单里每个文件自己的 seed 加密，和存储密钥无关。

把缓存搬到另一台设备时，要带上资源目录库、主数据库和它们的存储密钥，模块会在 Unity 读取 PlayerPrefs 之前写入。打包工具默认会**换成新生成的存储密钥**：两个库各用一个新 seed 重新加密、改名，`app` 库不带。这样包里的密钥不属于任何一次安装。

## Android：制作资源包

需要能读取游戏私有目录的环境（root 设备或模拟器），先打包两个 tar：

```sh
# 外部文件（资源缓存）
adb shell su -c 'tar -C /sdcard/Android/data/com.oddno.lovelive/files -cf /sdcard/game-external.tar D M'
# 私有文件（只用其中的 PlayerPrefs 存储密钥）
adb shell su -c 'tar -C /data/data/com.oddno.lovelive -cf /sdcard/game-private.tar shared_prefs'
adb pull /sdcard/game-external.tar /sdcard/game-private.tar .
```

```sh
python3 -m tools.linkura_offline android-resource-pack-build \
  --snapshot game-external.tar [--delta 之后的增量.tar ...] \
  --private game-private.tar --output <资源包目录>
```

资源包包含 `D/`、换了新密钥的资源目录库和主数据库（`M/` 里只有这两个文件）和对应的存储密钥（`files/linkura-local/storage-key.json`），并带一份 `MANIFEST.json`。换密钥需要 SQLCipher 4 命令行工具（`brew install sqlcipher`）；`--keep-source-key` 保留原来的全部 `M/` 和原安装的密钥。输出目录不要放在 exFAT 上（macOS 会生成 `._*` 文件）。导入见 [Android](android.md#android-import-resources)。

只想导出存储密钥：`python3 -m tools.linkura_offline android-storage-key --prefs <playerprefs.xml> --output storage-key.json`。

已有的缓存（比如以前做的资源包、iOS 容器）也可以单独换密钥：

```sh
python3 -m tools.linkura_offline cache-rekey \
  --m-dir <缓存的 M/> --storage-key <对应的 storage-key.json> --output <新目录>
```

输出 `<新目录>/M/` 下的两个库和 `<新目录>/storage-key.json`，用它们替换原来的 `M/` 和 storage-key.json。storage-key.json 里没有主数据库那一项（比如 iOS 设备上导出的）时只处理资源目录库，并给出警告：这样的缓存在新设备上没有主数据。

<a id="ios-from-android"></a>

## iOS：从 Android 缓存转换

iOS 客户端读取同样的缓存结构和存储密钥，但 Unity 不接受别的平台构建的资源包，Android 资源包里也没有 Metal 着色器：

```sh
python3 tools/resources/android_to_ios.py \
  --cache <包含 D/ 和 M/ 的目录（例如资源包的 files/）> \
  --storage-key <资源包的 files/linkura-local/storage-key.json> \
  --ipa <已解密的 5.0.1 IPA> \
  --output <iOS 数据目录>
```

- 所有 UnityFS 资源包的平台标记从 Android（13）改成 iOS（9）；
- 给了 `--ipa` 时，资源包里的着色器换成 IPA 中同名的 Metal 着色器，或者一个近似的替代着色器（否则材质显示为洋红色）。这一步要用 UnityPy 解析每个资源包，比较慢；
- 其他文件原样复制；资源目录库和主数据库换成新密钥（同上，`--keep-source-key` 关闭）；中断后重跑会跳过已转换的文件，并沿用第一次生成的密钥。

输出的 `container/` 可以直接交给 `tools/ios_device_setup.py --backup`。

## 检查缓存覆盖率

```sh
python3 tools/resources/hailstorm.py coverage --catalog <catalog.json> --cache <包含 D/ 的目录>
```

按类别列出资源清单里有多少文件在缓存中、缺多少 GB。下载失败会留下 0–28 字节的占位文件，按缺失统计。`catalog.json` 是 Hailstorm 的资源清单（例如 inspix-hailstorm 下载时保存的那份）。

加密的 raw 资源（ResourceType 192，例如剧情剧本 `story_main_*.txt`）可以解密查看：

```sh
python3 tools/resources/hailstorm.py decrypt --catalog <catalog.json> --cache <目录> \
  --label story_main_10250101.txt --output story.txt
```
