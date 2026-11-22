<!-- Translation source: 1a59167456efd164e163fbb74cc42e36365b3580 (docs/zh/hooking-android.md) -->

# Android Hook

ネイティブモジュールは ShadowHook で inline hook を行い、UnityResolve と IL2CPP API でクラスとメソッドを**名前で**探します。そのため固定アドレスに依存せず、クライアントの小さな更新では通常変更が不要です。

## Hook の作成

```cpp
// hooks/HookShare.cpp または hooks/HookOfflineFixes.cpp
DEFINE_HOOK(int32_t, CommonApiCache_get_CurrentChapterRank, (void* self, void* mtd)) {
    const auto rank = CommonApiCache_get_CurrentChapterRank_Orig(self, mtd);
    return rank;
}

void Install(HookInstaller* hookInstaller) {
    ADD_HOOK(CommonApiCache_get_CurrentChapterRank, Il2cppUtils::GetMethodPointer(
            "Assembly-CSharp.dll", "Tecotec", "CommonApiCache", "get_CurrentChapterRank"));
}
```

- `DEFINE_HOOK(戻り値の型, 名前, (引数...))` は `名前_Hook` と元の関数ポインター `名前_Orig` を生成します。インスタンスメソッドの最初の引数は `self`、最後は `MethodInfo*` です。
- `ADD_HOOK(名前, アドレス)` で導入し、ログに `ADD_HOOK: 名前 at 0x...` が出ます。
- オフライン時だけ有効にする処理は `Config::enableOfflineApiMock` で判定します。

## よく使う検索方法

| 用途 | 書き方 |
|---|---|
| 通常のメソッド | `Il2cppUtils::GetMethodPointer(アセンブリ, 名前空間, クラス名, メソッド名)` |
| 引数の数を指定 | `Il2cppUtils::GetMethodIl2cpp(klass, "メソッド名", 引数の数)` |
| async ステートマシン | `Il2cppUtils::find_nested_class(klass, 判定関数)` で `"<メソッド名>d__"` の接頭辞に一致するものを探し、`MoveNext` を取得します |
| フィールドのオフセット | `Il2cppUtils::il2cpp_class_get_field_from_name(klass, "<>1__state")` |
| 内部呼び出し（icall） | `Il2cppUtils::il2cpp_resolve_icall("UnityEngine.Application::get_version")` |

async ステートマシンの番号（`d__6` の 6）はバージョンによって変わる可能性があるため、接頭辞で探します。クラス名とメソッド名は iOS のシンボル表（`tools/re/il2cpp_re.py find`）でも検索でき、両プラットフォームの C# コードはほぼ同じです。

## 既存モジュール

| ファイル | 内容 |
|---|---|
| `hooks/HookShare.cpp` | API 呼び出し、レスポンスヘッダー、アセットバージョン、危険な操作の保護、Chapter RANK、プッシュトークンの代替 |
| `hooks/HookOfflineFixes.cpp` | オフライン時の画面遷移、Home、ミッション一覧、async 例外処理、ログイン・ガチャ・画面の診断ログ |
| `hooks/HookSqlCipher.cpp` | マスターデータDBへのアクセス診断（既定では出力しません。`traceMasterImport`、`dumpSqlCipherKey` で有効化） |
| `hooks/HookVirtualClock.cpp` | 仮想時計（`enableVirtualClock` + `virtualClockUnixTime`）、既定ではオフ |

`HookOfflineFixes.cpp` にはログだけの Hook も常に導入されています。一部は他の Hook が使う状態フィールドを記録するため、個別に確認するまではデバッグ時限定に変更しないでください。
