# Android：构建与安装

模块是一个 Xposed/LSPatch 模块。LSPatch 把它和游戏 APK 封装在一起，安装后不需要 root，也不需要额外的管理 App。

## 1. 构建模块

```sh
cd android
JAVA_HOME=<JDK 17> ./gradlew :app:assembleDebug
# 输出 android/app/build/outputs/apk/debug/app-debug.apk
```

原生部分（`android/app/src/main/cpp`）会一起编进离线服务器（`server/src`）和内置数据（`server/data`、`server/db`）。

## 2. 封装游戏 APK

```sh
python3 -m tools.linkura_offline android-repack \
  --java <Java 21+ 的 java> \
  --source <你的 5.1.0 APK> \
  --output Linkura-5.1.0-offline.apk
```

- 默认使用 `android/lspatch/lspatch_loaderfix.jar`：LSPatch 0.7 加上静态 `xposed.dummy` 桩，避免全新安装后第一次启动崩溃（原因见 `android/lspatch/loaderfix/build.sh`）。
- `--source` 也可以是已经用 LSPatch 封装过的 APK，工具会取出里面的原始 APK 重新封装。
- 输出旁边的 `.json` 记录了原始 APK、模块和产物的哈希。

## 3. 安装

```sh
adb install -r Linkura-5.1.0-offline.apk
```

已安装官方客户端时，签名不同，需要先卸载（会丢失官方客户端的数据，先备份）。

<a id="android-import-resources"></a>

## 4. 导入资源

资源包的制作见 [准备资源](resources.md)。导入：

```sh
python3 -m tools.linkura_offline android-resource-pack-install --pack <资源包目录> --serial <adb serial>
```

工具会先启动一次游戏，让模块建好缓存目录（Android 11+ 的 adb 不能在别的应用的外部目录里新建子目录），然后用普通 adb 推送（约 11 分钟）并核对文件数。

## 5. 游玩

开启飞行模式（可选），启动游戏，标题页点 LOGIN。冷启动后标题页需要十几秒才响应。

## 配置和调试

- 内置配置在模块 APK 的 `assets/offline-config/`：`l4-config.json`（开关）、`client-res.json`（客户端与资源版本）、`archive-config.json`（存档）。
- 不重新打包就想改：把同名文件放到设备上游戏的 `files/linkura-local/config/`，优先使用它。
- 日志：`adb logcat -s LinkuraOffline LinkuraLocal-Native`。`l4-config.json` 里的 `dbgMode` 打开更多日志，`dumpHttpMockJson` 记录每个响应。
- 离线玩家库：游戏的 `files/HasuKikaisann.sqlite3`。在 `files/db_cmd` 写入 `reset`（清空，下次启动重新生成博物馆存档）或 `rebuild`（按内置数据重建、保留玩家状态），下次启动时执行。

## 精简说明

本模块只保留离线游玩需要的部分。原 linkura-localify 的自由镜头、翻译与字体替换、渲染设置、剧情显示开关、设置 App 和悬浮窗都已移除；因此画面使用游戏默认分辨率和字体。
