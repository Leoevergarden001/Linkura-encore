# 远程资源服务器（iOS，可选）

默认情况下，iOS 离线版的全部资源都推送到设备上（见 [iOS 构建与安装](ios.md)）。也可以把资源放在自己的服务器或对象存储上，由 App 首次使用时下载：设备不必一次推送二十多 GB，以后发布新的资源版本也不用再连电脑。

这只是资源的取得方式不同。API 和玩家存档仍然全部在设备上的离线服务器里处理，不需要联网。

## 工作方式

```
游戏（Unity）── http://127.0.0.1:52680/a/<路径> ──▶ dylib 里的离线服务器
                                                   ├─ Documents/LinkuraOffline/assets/<路径> 有文件：直接返回
                                                   └─ 否则：GET <remoteAssetBase>/<路径>（NSURLSession，HTTPS）
```

- IPA 的 metadata 已把官方资源地址改成 `http://127.0.0.1:52680/a`。Unity 只允许对本机回环地址使用明文 HTTP，所以游戏本身始终只访问本机，由离线服务器代为请求远程服务器。
- `/v1/archive/` 系列 API 响应里的官方 CDN 地址（封面、视频播放列表、动捕回放入口）会被改写成本机的 `/a/` 地址，也经过同一个代理。
- 代理只转发远程返回的 200 和 206；其他情况一律回复**不带内容的 404**。游戏的下载器不看状态码，会把响应内容写进缓存文件再按长度续传，带内容的错误页会永久损坏那个缓存文件。

## 远程服务器上的路径

远程服务器按游戏请求的路径提供文件，`<remoteAssetBase>` 之后的部分与 `/a/` 之后的部分相同：

| 路径 | 内容 |
|---|---|
| `raw/<前两位>/<RealName>` | 资源目录文件，以及媒体（`.acb`、`.awb`、`.usm`）和原始数据（主数据 `.tsv`、剧本等），ResourceType 128/192 |
| `ios/<前两位>/<RealName>` | iOS 资源包（ResourceType 1） |
| `archive/alst/…` | 动捕回放（可选）：入口 `.md` 的 `path` 字段需改成 `http://127.0.0.1:52680/a/archive/…`，让播放列表和分片也经过代理 |
| `archive/hls/<目录>/…` | 视频回放的 HLS 播放列表（可选） |
| `with_meets/`、`feslive/`、`with_station/` 下的 `thumbnail_image/*.jpg` | 存档封面 |
| `version.json` | 发布的资源版本，见下文 |

RealName 由资源目录决定。资源目录、`RealName` 和缓存文件的关系见 [准备资源](resources.md)。

## config.json

写在 App 的 `Documents/LinkuraOffline/config.json`：

```json
{
  "port": 52680,
  "resVersion": "R2606002@...",
  "remoteAssetBase": "https://<你的资源服务器>/a",
  "remoteAssetKey": "<这台设备的密钥>",
  "motionReplays": false
}
```

| 字段 | 作用 |
|---|---|
| `remoteAssetBase` | 远程资源的基础地址。设置后启用代理，启动时还会做一次连通性自检（结果写在 `server.log`） |
| `remoteAssetKey` | 可选。每个代理请求都会带上请求头 `x-linkura-key: <值>`，供服务器端拒绝没有密钥的请求（例如 CDN 的防火墙规则）。建议每台设备一个密钥，泄露时只需吊销那一个 |
| `resVersion` | 资源版本。设置了 `remoteAssetBase` 时，启动时会读取 `<remoteAssetBase>/version.json`（`{"resVersion": "..."}`），以其中的值为准；读不到时用这里的值。响应最多等到启动后 6 秒，不会卡住游戏 |
| `motionReplays` | 可选，默认 `true`。设为 `false` 时，With×MEETS 存档详情去掉 `archive_url` 并补回官方 `video_url`，改走视频回放；用于远程服务器上没有动捕数据的情况。Fes×LIVE 客户端固定走 3D 回放，不受影响 |

发布新的资源版本时，只需把新文件和新的资源目录放上远程服务器，再更新 `version.json`，设备下次启动就会同步新目录、只下载有变化的文件。

## 代理的传输方式

- **分段流式转发**：大文件按 4 MB 一段用 Range 请求远程服务器，边取边写回游戏，并在发送当前段的同时预取下一段。每个传输最多占两段内存，418 MB 的剧情影片也不会整个读进内存。中途失败时断开连接，游戏下载器会自己用 Range 续传。
- **客户端的 Range** 原样遵守（包括 `bytes=a-` 和 `bytes=a-b`），响应带正确的 `content-range`。
- **并发**：代理最多同时向远程服务器开 12 个连接，与下面的下载并发补丁一致。

## 下载并发

游戏的下载器（`Hailstorm.StardustDownloadWorker` → 原生库 `Blitz.Stardust`）由 `AdaptiveConcurrencyController` 决定同时下载几个文件：一张 1/2/3/4 个的静态表，测得吞吐量超过 80/130/180/260 Mbit/s 才升一级。远程服务器离得远时（每个请求 0.2～0.6 秒），吞吐量到不了第一级，客户端就一个一个串行下载，五万多个文件的全新安装要好几个小时。

`ios-offline-stage` 默认打两处补丁：`RecommendedDownloaderNum` 固定返回 12，原生下载器的上限从 4 改成 12。实测从每秒约 2 个文件、0.9 MB/s 提升到每秒约 17 个文件、11 MB/s。打包时加 `--keep-download-concurrency` 可保留原行为。

## 视频回放的播放列表

存档的视频回放是 Mux 生成的 HLS：每档码率一个 `.m4s` 文件，播放列表用 `EXT-X-BYTERANGE` 指定每段的字节范围。只要知道每个 `.m4s` 在某个公开可访问的 HLS 归档里的位置，就可以生成**静态**播放列表：主播放列表只保留一档画质和主音轨，媒体播放列表里的 URI 改成那个归档文件的绝对地址，字节范围加上偏移。这样远程服务器只需托管几十 MB 的播放列表，视频分片由播放器直接从归档地址取得。

播放器直接访问归档地址，所以这些分片请求不带 `x-linkura-key`；需要密钥保护的只有经过代理的播放列表。

## 在电脑上测试

主机版服务器支持同样的代理，用 curl 代替 NSURLSession：

```sh
server/build-host.sh
server/build/host/linkura-offline-server --data /tmp/offline-data \
  --remote-asset-base https://<你的资源服务器>/a --remote-asset-key <密钥> \
  [--remote-chunk-bytes 4194304] [--res-version-file version.txt] [--motion-replays off]
```

`server/tests/smoke_test.py` 用一个本地的 Range 服务器检查：密钥请求头、分段流式转发、客户端 Range、不带内容的 404、动态 resVersion，以及 `motionReplays` 关闭时的视频回放。
