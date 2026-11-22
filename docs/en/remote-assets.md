<!-- Translation source: 77dbd449d49a1c8a20703f7b2da796dad2fa0411 (docs/zh/remote-assets.md) -->

# Remote asset host (iOS, optional)

By default, every asset of the iOS offline build is pushed to the device (see [iOS build and install](ios.md)). Assets can instead live on your own server or object storage, and the app downloads each one the first time it is needed. The device then does not need twenty-odd GB pushed at once, and publishing a new asset version no longer needs a computer.

Only the way assets are obtained changes. The API and the player save are still handled entirely by the offline server on the device, without a network.

## How it works

```
Game (Unity) ── http://127.0.0.1:52680/a/<path> ──▶ offline server in the dylib
                                                   ├─ file in Documents/LinkuraOffline/assets/<path>: served as is
                                                   └─ otherwise: GET <remoteAssetBase>/<path> (NSURLSession, HTTPS)
```

- The IPA's metadata already points the official asset base at `http://127.0.0.1:52680/a`. Unity allows plain HTTP only towards loopback, so the game itself only ever talks to the device, and the offline server makes the remote requests.
- Official CDN URLs in `/v1/archive/` responses (covers, video playlists, motion replay entries) are rewritten to local `/a/` URLs and go through the same proxy.
- The proxy passes on only 200 and 206 from the remote; anything else becomes a **bodiless 404**. The game's downloader ignores the status, writes the body into the cache file and resumes from its length, so an error page with a body would corrupt that cache file for good.

## Paths on the remote host

The remote host serves files at the paths the game requests: the part after `<remoteAssetBase>` equals the part after `/a/`.

| Path | Content |
|---|---|
| `raw/<first two>/<RealName>` | The catalog file, media (`.acb`, `.awb`, `.usm`) and raw data (masterdata `.tsv`, story scripts, ...), ResourceType 128/192 |
| `ios/<first two>/<RealName>` | iOS asset bundles (ResourceType 1) |
| `archive/alst/…` | Motion replays (optional): the `path` field of each `.md` entry must point at `http://127.0.0.1:52680/a/archive/…`, so playlists and segments also go through the proxy |
| `archive/hls/<dir>/…` | HLS playlists for video replays (optional) |
| `thumbnail_image/*.jpg` under `with_meets/`, `feslive/` and `with_station/` | Archive covers |
| `version.json` | The published asset version, see below |

The catalog determines each RealName. See [prepare assets](resources.md) for how the catalog, `RealName` and cache files relate.

## config.json

In the app's `Documents/LinkuraOffline/config.json`:

```json
{
  "port": 52680,
  "resVersion": "R2606002@...",
  "remoteAssetBase": "https://<your asset host>/a",
  "remoteAssetKey": "<this device's key>",
  "motionReplays": false
}
```

| Field | Purpose |
|---|---|
| `remoteAssetBase` | Base URL of the remote assets. Setting it enables the proxy and a connectivity self-test at startup (result in `server.log`) |
| `remoteAssetKey` | Optional. Every proxied request carries the header `x-linkura-key: <value>`, so the host can refuse requests without a key (for example with a CDN firewall rule). Use one key per device, so a leaked key can be revoked alone |
| `resVersion` | Asset version. With `remoteAssetBase` set, the server reads `<remoteAssetBase>/version.json` (`{"resVersion": "..."}`) at startup and uses that value; if it cannot, it uses this one. Responses wait for it at most until 6 s after launch, so the game never hangs on it |
| `motionReplays` | Optional, default `true`. With `false`, With×MEETS archive details drop `archive_url` and carry the official `video_url`, so they play the video replay; for hosts without motion data. Fes×LIVE always opens the 3D replay and is not affected |

To publish a new asset version, put the new files and the new catalog on the host and update `version.json`. On its next start the device syncs the new catalog and downloads only the files that changed.

## How the proxy transfers

- **Streamed pieces**: large files are requested from the host in 4 MB Range pieces and written back to the game as they arrive; the next piece is fetched while the current one is sent. A transfer holds at most two pieces, so even a 418 MB story movie never sits in memory whole. If a transfer breaks, the connection is closed and the game's downloader resumes with a Range request.
- **The client's Range** is honoured as given (`bytes=a-` and `bytes=a-b`) with a correct `content-range`.
- **Concurrency**: the proxy opens at most 12 connections to the host, matching the download concurrency patch below.

## Download concurrency

The game's downloader (`Hailstorm.StardustDownloadWorker` → native library `Blitz.Stardust`) asks `AdaptiveConcurrencyController` how many files to download at once: a static table of 1/2/3/4, stepped up only when measured throughput passes 80/130/180/260 Mbit/s. Against a distant host (0.2-0.6 s per request) throughput never reaches the first step, so the client downloads one file at a time, and a fresh install of over fifty thousand files takes many hours.

`ios-offline-stage` applies two patches by default: `RecommendedDownloaderNum` returns a fixed 12, and the native downloader's cap goes from 4 to 12. Measured: from about 2 files and 0.9 MB/s to about 17 files and 11 MB/s per second. Pass `--keep-download-concurrency` to keep the original behaviour.

## Playlists for video replays

Archive video replays are Mux HLS: one `.m4s` file per rendition, with each segment given as an `EXT-X-BYTERANGE`. If you know where each `.m4s` sits inside a publicly reachable HLS archive, you can generate **static** playlists: the master playlist keeps one rendition and the main audio, and the media playlists point their URIs at that archive file's absolute URL with byte ranges shifted by the offset. The host then serves only a few tens of MB of playlists, and the player fetches the segments from the archive itself.

The player fetches those segments directly, without `x-linkura-key`; only the playlists, which go through the proxy, are key-protected.

## Testing on a computer

The host build of the server supports the same proxy, using curl instead of NSURLSession:

```sh
server/build-host.sh
server/build/host/linkura-offline-server --data /tmp/offline-data \
  --remote-asset-base https://<your asset host>/a --remote-asset-key <key> \
  [--remote-chunk-bytes 4194304] [--res-version-file version.txt] [--motion-replays off]
```

`server/tests/smoke_test.py` checks, against a local Range server: the key header, streamed pieces, the client's Range, the bodiless 404, the dynamic resVersion, and video replays with `motionReplays` off.
