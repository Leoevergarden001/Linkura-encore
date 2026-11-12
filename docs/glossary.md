# Terminology · 术语 · 用語

Use these terms consistently in the three documentation languages. Keep names shown in the game UI in Japanese, including `キャッシュ削除`, `プレイPt.`, and `Chapter RANK`. Do not translate commands, identifiers, paths, addresses, or hexadecimal values.

三种语言的文档统一使用下表术语。游戏界面文字保留日文原文；命令、标识符、路径、地址和十六进制值不翻译。

3 言語の文書では、以下の用語を統一して使います。ゲーム画面の表記は日本語のままにし、コマンド、識別子、パス、アドレス、16 進数の値は翻訳しません。

| 中文 | English | 日本語 |
|---|---|---|
| 存储密钥 | storage key | ストレージキー |
| 资源目录库 | catalog database | カタログDB |
| 主数据库 | master database | マスターデータDB |
| 资源包 | resource pack | リソースパック |
| 资源缓存 | asset cache | アセットキャッシュ |
| 离线服务器 | offline server | オフラインサーバー |
| 玩家库 | player database | プレイヤーDB |
| 博物馆存档 | museum save | ミュージアムセーブデータ |
| 内置数据 | embedded data | 内蔵データ |
| 响应模板 | response template | レスポンステンプレート |
| 静态补丁 | static patch | 静的パッチ |
| 封装 / 重新封装 | repack | 再パッケージ化 |
| 已解密 IPA | decrypted IPA | 復号済み IPA |
| 描述文件 | provisioning profile | プロビジョニングプロファイル |
| 回归检查 | regression check | 回帰確認 |
| 覆盖文件 | override file | 上書きファイル |

## Translation maintenance · 翻译维护 · 翻訳の保守

`docs/zh/` and `README.zh-CN.md` are the source text. The English and Japanese files carry a `Translation source` comment with the Chinese source commit. After changing Chinese, review both translations and update their markers to the new source commit. Commit source changes first, then translations, so the marker can name an existing commit.

中文原稿改动后，先提交原稿，再核对英文和日文并更新译文中的源提交标记。`python3 tools/check_docs.py` 检查三种语言的文件名、相对链接和锚点；原稿晚于标记提交发生变化时只给出提醒，不使 CI 失败。

中国語の原稿を変更したら先にコミットし、英語版と日本語版を確認して参照コミットを更新してください。`python3 tools/check_docs.py` はファイル名、相対リンク、アンカーを検査します。原稿が参照コミットより新しい場合は警告だけを出し、CI は失敗させません。
