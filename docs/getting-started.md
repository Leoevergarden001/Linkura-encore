# 入门

## 支持的版本

| 平台 | 客户端 | 资源版本 | 说明 |
|---|---|---|---|
| Android | 5.1.0（versionCode 109563） | `R2606002@RQMqABAxdj0b5c8TKh4+PNDUYQ==` | 停服前最后一个在线版本。模块按名字查找类和方法，不依赖地址 |
| iOS / iPadOS | 5.0.1 | 同上（用 5.1.0 的资源） | 补丁按地址写死，并逐条校验原始字节；只认这一个构建（解密 IPA 的 SHA-256 `5d65a125b7701f51…`） |

资源版本决定客户端接受哪一份缓存，必须和你的缓存一致。默认值在 `android/app/src/main/assets/offline-config/client-res.json`。

## 你需要自己准备的

1. **客户端**：Android 5.1.0 APK；iOS 需要**已解密**的 5.0.1 IPA（App Store 下载的 IPA 是加密的）。
2. **资源缓存**：游戏在设备上下载过的资源，即应用外部文件目录里的 `D/`（资源）和 `M/`（主数据库），以及与之配对的**存储密钥**（PlayerPrefs 中的一项）。没有存储密钥，客户端会把缓存当成别人的而忽略。见 [准备资源](resources.md)。
3. **工具链**：
   - 离线服务器的主机版：C++23 编译器（clang 16+ / gcc 13+）和 CMake（只用来生成内置数据头文件）。
   - Android：Android SDK、NDK 26.3、JDK 17；封装用的 LSPatch 需要 Java 21+。
   - iOS：Xcode（iPhoneOS SDK），Apple 账号（免费 Personal Team 可用，签名 7 天有效）。
   - Python 3.10+；数据生成需要 PyYAML，资源工具需要 `lz4`、`cryptography`，iOS 资源转换还需要 `UnityPy`。

## 获取代码

```sh
git clone --recurse-submodules <repo>
# 已经克隆过：
git submodule update --init
```

`third_party/link-like-diff` 是游戏主数据（YAML），只有重新生成内置数据时需要。

## 下一步

- Android：[构建与安装](android.md)
- iOS / iPadOS：[构建与安装](ios.md)
- 想先在电脑上看离线服务器怎么工作：`server/build-host.sh && python3 server/tests/smoke_test.py`，见 [离线服务器](server.md)。

## 隐私

从设备上导出的 PlayerPrefs、设备备份和其中的 `M/` 数据库属于那一次安装，`app` 库里还有原账号的玩家 ID。打包工具默认会把资源目录库和主数据库换成新生成的存储密钥、不带 `app` 库（见 [准备资源](resources.md)），但原始导出和备份仍然只应该留在你自己的硬盘上。资源包本身是游戏内容，同样不要分享。所有这些都不要提交到任何仓库：仓库的 `.gitignore` 不会替你挡住放错位置的文件，提交前请自己检查。
