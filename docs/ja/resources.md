<!-- Translation source: 1a59167456efd164e163fbb74cc42e36365b3580 (docs/zh/resources.md) -->

# アセットの準備

公式 CDN は停止しています。アセットは、ご自身の端末でゲームがダウンロードしていたキャッシュから用意してください。

## キャッシュの内容

ゲームの外部ファイルディレクトリは、Android では `/sdcard/Android/data/com.oddno.lovelive/files`、iOS ではアプリコンテナの `Documents/` です。

| パス | 内容 |
|---|---|
| `D/<2文字の接頭辞>/<RealName>` | アセット（Hailstorm キャッシュ）：UnityFS バンドル、音声（`acb`/`awb`）、動画（`usm`）、暗号化テキストなど |
| `M/` | データベース（SQLCipher） |

`M/` のデータベースは**ストレージキー**から名前と暗号化キーが決まります。クライアントはストレージ領域ごとに 256 バイトのランダムな seed を PlayerPrefs に保存します（Android では `shared_prefs/com.oddno.lovelive.v2.playerprefs.xml`）。

| ストレージ領域 | PlayerPrefs 名 | `M/` のデータベース | オフライン版で必要か |
|---|---|---|---|
| `//catalog` | `JWbAatNSI1NpHhCdU%2BgJllY0BTU%3D` | カタログDB（各アセットが `D/` のどのファイルに対応するか） | 必要 |
| `//master` | `Dr%2BcmYmVx%2B1kQ8uTmoYADAduWVk%3D` | マスターデータDB（約 57 MB） | 必要。クライアントはキャッシュから空のマスターデータDBを再生成できません。 |
| `app` | — | プレイヤー ID、ローカル設定など | 不要。元のアカウント情報があるため含めません。 |

- データベース名 = 小文字でパディングなしの Base32(MD5(seed)) です。
- SQLCipher 4 の raw key = SHA-256(seed) です（両データベース共通の計算方法）。
- `D/` のアセットはカタログ内のファイル固有 seed で暗号化され、ストレージキーとは無関係です。

キャッシュを別の端末に移すときは、カタログDB、マスターデータDB、両方のストレージキーを一緒に持っていきます。モジュールは Unity が PlayerPrefs を読む前にキーを書き込みます。パッケージ化ツールは既定で**新しいストレージキーに置き換え**、両データベースを別々の新しい seed で再暗号化・改名し、`app` データベースを除きます。そのため、パック内のキーは元のインストールに属しません。

## Android：リソースパックの作成

ゲームのプライベートディレクトリを読める環境（root 化した端末またはエミュレーター）で、まず二つの tar を作ります。

```sh
# 外部ファイル（アセットキャッシュ）
adb shell su -c 'tar -C /sdcard/Android/data/com.oddno.lovelive/files -cf /sdcard/game-external.tar D M'
# プライベートファイル（PlayerPrefs のストレージキーだけを使用）
adb shell su -c 'tar -C /data/data/com.oddno.lovelive -cf /sdcard/game-private.tar shared_prefs'
adb pull /sdcard/game-external.tar /sdcard/game-private.tar .
```

```sh
python3 -m tools.linkura_offline android-resource-pack-build \
  --snapshot game-external.tar [--delta <後続の差分.tar> ...] \
  --private game-private.tar --output <リソースパックのディレクトリ>
```

リソースパックには `D/`、新しいキーに置き換えたカタログDBとマスターデータDB（`M/` にあるのはこの二つだけ）、対応するストレージキー（`files/linkura-local/storage-key.json`）、`MANIFEST.json` が入ります。キーの置き換えには SQLCipher 4 CLI（`brew install sqlcipher`）が必要です。`--keep-source-key` は元の `M/` とキーをそのまま保持します。exFAT を出力先にしないでください（macOS が `._*` ファイルを作ります）。導入は [Android](android.md#android-import-resources) を参照してください。

ストレージキーだけを取り出す場合：`python3 -m tools.linkura_offline android-storage-key --prefs <playerprefs.xml> --output storage-key.json`。

既存のキャッシュ（以前作ったリソースパックや iOS コンテナなど）も単独でキーを置き換えられます。

```sh
python3 -m tools.linkura_offline cache-rekey \
  --m-dir <キャッシュの M/> --storage-key <対応する storage-key.json> --output <新しいディレクトリ>
```

出力された `<新しいディレクトリ>/M/` の二つのデータベースと `<新しいディレクトリ>/storage-key.json` を元の `M/` と `storage-key.json` に置き換えます。元の `storage-key.json` にマスターデータDBの項目がない場合（iOS 端末からの書き出しなど）は、カタログDBだけを処理して警告します。そのキャッシュだけでは新しい端末にマスターデータを用意できません。

<a id="ios-from-android"></a>

## iOS：Android キャッシュからの変換

iOS クライアントは同じキャッシュ構成とストレージキーを読みますが、Unity は別プラットフォーム向けのアセットバンドルを拒否します。また、Android バンドルには Metal シェーダーがありません。

```sh
python3 tools/resources/android_to_ios.py \
  --cache <D/ と M/ を含むディレクトリ（例：リソースパックの files/）> \
  --storage-key <リソースパックの files/linkura-local/storage-key.json> \
  --ipa <復号済みの 5.0.1 IPA> \
  --output <iOS データディレクトリ>
```

- すべての UnityFS バンドルのプラットフォーム値を Android（13）から iOS（9）に変更します。
- `--ipa` を指定すると、バンドル内のシェーダーを IPA 内の同名 Metal シェーダー、または近い代替シェーダーに置き換えます。そうしないとマテリアルがマゼンタ色になります。各バンドルを UnityPy で解析するため時間がかかります。
- その他のファイルはそのままコピーし、カタログDBとマスターデータDBは新しいキーに置き換えます（`--keep-source-key` で無効化）。再実行では変換済みファイルを飛ばし、初回に生成したキーを再利用します。

出力された `container/` を `tools/ios_device_setup.py --backup` に渡します。

## キャッシュの網羅率の確認

```sh
python3 tools/resources/hailstorm.py coverage --catalog <catalog.json> --cache <D/ を含むディレクトリ>
```

カテゴリごとに、カタログ内で保存済みのファイル数と不足容量を表示します。ダウンロード失敗時に残る 0–28 バイトのプレースホルダーは不足として数えます。`catalog.json` は、たとえば inspix-hailstorm で保存した Hailstorm のアセットカタログです。

暗号化された raw アセット（ResourceType 192）、たとえば `story_main_*.txt` の脚本は復号して確認できます。

```sh
python3 tools/resources/hailstorm.py decrypt --catalog <catalog.json> --cache <ディレクトリ> \
  --label story_main_10250101.txt --output story.txt
```
