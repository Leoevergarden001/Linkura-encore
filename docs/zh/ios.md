# iOS / iPadOS：构建与安装

未越狱的 iOS 不能在运行时 Hook，所以离线运行在数据和代码两个层面完成：

- `LinkuraOffline.dylib` 打进 IPA 的 `Frameworks/`，通过主程序的 `LC_LOAD_DYLIB` 加载，在 `127.0.0.1:52680` 运行离线服务器；
- IL2CPP metadata 里的 API 和资源地址改成本机地址；
- 对 UnityFramework 做按原始字节校验的静态补丁（见 [iOS 补丁](patching-ios.md)）；
- Info.plist 换成独立的 Bundle ID，允许明文 HTTP，打开文件共享。

## 1. 构建 dylib

```sh
ios/build.sh                 # -> ios/build/LinkuraOffline.dylib
LINKURA_REMOTE=0 ios/build.sh  # 不带开发用的远程点击/截图工具
```

## 2. 生成未签名 IPA

```sh
python3 -m tools.linkura_offline ios-offline-stage \
  --source <已解密的 5.0.1 IPA> \
  --show-preview-bgm-fallback --disable-lyric-videos \
  --output Linkura-iOS-5.0.1-offline-unsigned.ipa
```

默认 Bundle ID 是 `org.linkura.preservation.probe`（`--bundle-id` 可改）。默认开启的补丁和关闭方式见 `--help`。每处补丁先核对原始字节，不匹配就停止，不会生成不完整的包。

## 3. 签名、安装、推送资源（一步完成）

设备用数据线连接 Mac、点"信任"、开启开发者模式（设置 → 隐私与安全性 → 开发者模式）。设备要登记在描述文件里：在 Xcode 里用同一个 Bundle ID 和你的 Team 运行任意 App 一次，Xcode 会自动登记并生成描述文件。

```sh
python3 tools/ios_device_setup.py \
  --backup <iOS 数据目录（包含 container/）> \
  --unsigned Linkura-iOS-5.0.1-offline-unsigned.ipa \
  --device "<设备名或 UDID>" --launch --port 8791
```

脚本会自动选择包含该设备、未过期的描述文件和唯一的签名身份，然后安装、推送资源缓存、主数据库和存储密钥，逐个比对文件大小，最后启动并确认离线服务已经监听。

- 资源约 21 GB：设备至少留出 22 GB；推送期间保持解锁、亮屏；中断后重跑会跳过已推送的文件。
- 只想看计划用 `--dry-run`，只校验用 `--verify-only`。
- 玩家进度库默认不推；`--include-progress` 推送，设备上已有时需 `--overwrite-progress`（先把旧库拉回本地）。
- 设备第一次安装这个签名身份的 App 时，要在设备上打开 设置 → 通用 → VPN 与设备管理，在“开发者 App”下信任它，否则客户端启动失败。安装后、推送期间就可以去信任；如果已经失败，信任后重跑同一条命令即可。
- 免费签名 7 天过期，过期后重跑同一条命令（覆盖安装保留数据）。

iOS 数据目录可以由 Android 缓存转换得到，见 [准备资源](resources.md#ios-from-android)。

只签名不安装：`tools/ios_sign_ipa.sh <unsigned.ipa> <profile.mobileprovision> "<签名身份>" <signed.ipa>`。

## 调试

dylib 把运行信息写在 App 的 `Documents/LinkuraOffline/`（文件共享已打开，Finder 和"文件"App 可见）：

| 文件 | 作用 |
|---|---|
| `server.log` | 每个请求的路径和状态码、未匹配的路由（`no route for`）、未提供的资源请求 |
| `config.json` | 可选：`{"port": 52680, "resVersion": "..."}` |
| `storage-key.json` | 启动前写入 PlayerPrefs 的存储密钥 |
| `mock_api/v1/<路径>.json`（及 `.headers`） | 可选：按路径覆盖响应，用于快速试验；只有开发版（不带 `LINKURA_REMOTE=0` 构建）读取，正式版忽略 |

玩家库在 `Library/Application Support/LinkuraOffline/HasuKikaisann.sqlite3`，首次启动自动生成博物馆存档。
