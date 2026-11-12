<!-- Translation source: 60f383ea76a885c71ddad0e2d24e2955fc4c6c78 (docs/zh/ios.md) -->

# iOS / iPadOS：ビルドとインストール

脱獄していない iOS では実行時の Hook を使えないため、データとコードの両方を変更してオフライン動作を実現します。

- `LinkuraOffline.dylib` を IPA の `Frameworks/` に組み込み、メイン実行ファイルの `LC_LOAD_DYLIB` で読み込みます。オフラインサーバーは `127.0.0.1:52680` で動きます。
- IL2CPP metadata 内の API とアセットのアドレスをローカルのアドレスに変更します。
- UnityFramework に元のバイト列を検証する静的パッチを適用します（[iOS パッチ](patching-ios.md)を参照）。
- Info.plist に独立した Bundle ID を設定し、平文 HTTP とファイル共有を有効にします。

## 1. dylib のビルド

```sh
ios/build.sh                 # -> ios/build/LinkuraOffline.dylib
LINKURA_REMOTE=0 ios/build.sh  # 開発用のリモート操作・撮影ツールを除外
```

## 2. 未署名 IPA の作成

```sh
python3 -m tools.linkura_offline ios-offline-stage \
  --source <復号済みの 5.0.1 IPA> \
  --show-preview-bgm-fallback --disable-lyric-videos \
  --output Linkura-iOS-5.0.1-offline-unsigned.ipa
```

既定の Bundle ID は `org.linkura.preservation.probe` です（`--bundle-id` で変更可能）。既定で有効なパッチと無効化方法は `--help` を参照してください。各パッチは書き込み前に元のバイト列を検証し、一致しなければ停止します。不完全な IPA は生成しません。

## 3. 署名・インストール・アセット転送

端末をケーブルで Mac に接続して「信頼」を選び、開発者モード（設定 → プライバシーとセキュリティ → デベロッパモード）を有効にしてください。端末をプロビジョニングプロファイルに登録する必要があります。同じ Bundle ID とご自身の Team で任意のアプリを Xcode から一度実行すると、Xcode が端末を登録してプロファイルを作成します。

```sh
python3 tools/ios_device_setup.py \
  --backup <container/ を含む iOS データディレクトリ> \
  --unsigned Linkura-iOS-5.0.1-offline-unsigned.ipa \
  --device "<端末名または UDID>" --launch --port 8791
```

スクリプトは、端末を含む有効期限内のプロファイルと一意の署名 ID を選びます。アプリをインストールし、アセットキャッシュ、マスターデータDB、ストレージキーを転送して各ファイルのサイズを比較し、最後に起動してオフラインサーバーの待ち受けを確認します。

- アセットは約 21 GB です。端末に 22 GB 以上の空きを確保し、転送中はロックを解除したまま画面を点灯してください。中断後の再実行では転送済みファイルを飛ばします。
- `--dry-run` で計画だけを表示し、`--verify-only` で検証だけを行えます。
- プレイヤーの進行状況は既定では転送しません。転送する場合は `--include-progress` を使います。端末に既存のプレイヤーDBがある場合は先に取り出してから `--overwrite-progress` を指定してください。
- この署名 ID のアプリを端末に初めてインストールしたときは、端末の 設定 → 一般 → VPNとデバイス管理 の「デベロッパApp」で信頼してください。信頼しないとアプリを起動できません。インストール直後、転送中に信頼できます。起動に失敗した場合も、信頼してから同じコマンドを再実行すれば続行できます。
- 無料署名は 7 日で期限切れになります。同じコマンドを再実行して上書きインストールするとデータを保持できます。

iOS 用データディレクトリは Android のキャッシュから変換できます。[アセットの準備](resources.md#ios-from-android)をご覧ください。

署名だけ行う場合：`tools/ios_sign_ipa.sh <unsigned.ipa> <profile.mobileprovision> "<署名 ID>" <signed.ipa>`。

## デバッグ

dylib は実行情報をアプリの `Documents/LinkuraOffline/` に書き込みます。ファイル共有を有効にしているため、Finder と「ファイル」アプリから確認できます。

| ファイル | 内容 |
|---|---|
| `server.log` | 各リクエストのパスと状態コード、未登録ルート（`no route for`）、不足アセットのリクエスト |
| `config.json` | 任意：`{"port": 52680, "resVersion": "..."}` |
| `storage-key.json` | 起動前に PlayerPrefs に書き込むストレージキー |
| `mock_api/v1/<パス>.json`（および `.headers`） | パスごとのレスポンス上書きファイル。素早い試験に使います。 |

プレイヤーDBは `Library/Application Support/LinkuraOffline/HasuKikaisann.sqlite3` にあります。初回起動時にミュージアムセーブデータを自動生成します。
