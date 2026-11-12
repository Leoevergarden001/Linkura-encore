<!-- Translation source: 501289cf3b617f15507c0039ea96fbc2e157595c (docs/zh/android.md) -->

# Android：ビルドとインストール

これは Xposed/LSPatch モジュールです。LSPatch がゲームの APK と一緒に再パッケージ化するため、インストール後に root や別の管理アプリは必要ありません。

## 1. モジュールのビルド

```sh
cd android
JAVA_HOME=<JDK 17> ./gradlew :app:assembleDebug
# 出力：android/app/build/outputs/apk/debug/app-debug.apk
```

ネイティブ部分（`android/app/src/main/cpp`）には、オフラインサーバー（`server/src`）と内蔵データ（`server/data`、`server/db`）も組み込まれます。

## 2. ゲーム APK の再パッケージ化

```sh
python3 -m tools.linkura_offline android-repack \
  --java <Java 21+ の java> \
  --source <手元の 5.1.0 APK> \
  --output Linkura-5.1.0-offline.apk
```

- 既定では `android/lspatch/lspatch_loaderfix.jar` を使います。LSPatch 0.7 に静的な `xposed.dummy` スタブを加え、新規インストール後の初回起動でクラッシュする問題を避けます（`android/lspatch/loaderfix/build.sh` を参照）。
- `--source` には LSPatch で再パッケージ化済みの APK も指定できます。ツールが元の APK を取り出して再パッケージ化します。
- 出力の隣に作られる `.json` には、元の APK、モジュール、生成物のハッシュが記録されます。

## 3. インストール

```sh
adb install -r Linkura-5.1.0-offline.apk
```

公式クライアントが既に入っている場合は署名が異なるため、先にアンインストールが必要です。データが消えるので、必ず先にバックアップしてください。

<a id="android-import-resources"></a>

## 4. アセットの導入

リソースパックの作り方は[アセットの準備](resources.md)を参照してください。導入コマンドは次のとおりです。

```sh
python3 -m tools.linkura_offline android-resource-pack-install --pack <リソースパックのディレクトリ> --serial <adb serial>
```

ツールは一度ゲームを起動してモジュールにキャッシュディレクトリを作らせます（Android 11+ では adb が他のアプリの外部ディレクトリに新しいサブディレクトリを作れません）。その後、通常の adb でファイルを転送し、ファイル数を確認します。転送は約 11 分です。

## 5. プレイ

必要なら機内モードを有効にし、ゲームを起動してタイトル画面の LOGIN をタップしてください。コールドスタート後はタイトル画面が反応するまで十数秒かかることがあります。

## 設定とデバッグ

- 内蔵設定はモジュール APK の `assets/offline-config/` にあります。`l4-config.json` はスイッチ、`client-res.json` はクライアントとアセットのバージョン、`archive-config.json` はセーブデータを設定します。
- 再パッケージ化せずに変更する場合は、同名のファイルをゲームの `files/linkura-local/config/` に置いてください。こちらが優先されます。
- ログは `adb logcat -s LinkuraOffline LinkuraLocal-Native` で確認できます。`l4-config.json` の `dbgMode` で詳しいログを、`dumpHttpMockJson` で全レスポンスの記録を有効にできます。
- プレイヤーDBは `files/HasuKikaisann.sqlite3` です。`files/db_cmd` に `reset` を書くと次回起動時に消去してミュージアムセーブデータを再生成し、`rebuild` を書くとプレイヤー状態を保持して内蔵データから再構築します。

## モジュールの簡素化

オフラインプレイに必要な機能だけを残しています。元の linkura-localify にあった自由カメラ、翻訳とフォントの置き換え、描画設定、ストーリー表示スイッチ、設定アプリ、フローティングウィンドウは削除しました。そのため、ゲーム既定の解像度とフォントを使います。
