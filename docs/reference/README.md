# 参考資料（開発メモ）

このフォルダの文書は、開発中の作業ツリーで書いた記録をそのまま収録したものです。
公開リポジトリとは構成が違うため、読むときは次の点に注意してください。

- `F:\DC_EVE`、`iso_root/`、`iso_stage/`、`scratch/`、`.claude/skills/` などのパスは開発環境のもので、
  このリポジトリにはありません。公開版の手順は [README](../../README.md) を見てください。
- 音声と BGM については、あとから分かったことで内容が古くなっています。
  - 音声（`SOUND/*.WAV`）は 6 ビット対数 DPCM で、文書中の「ADPCM として変換」は誤りでした。
  - BGM は Windows 版の MIDI ではなく、サターン版の `MUSIC/*.SEQ` から直接レンダリングする方式に変わりました。
  - 正しい内容は [formats.md](../formats.md) にあります。
- ゲームのデータは含みません。

| 文書 | 内容 |
|---|---|
| [dreamcast-homebrew-knowledge.md](dreamcast-homebrew-knowledge.md) | Dreamcast ホームブリュー開発で実際に踏んだ問題と解決策（PVR、サウンド、CD、ビルド環境、テスト自動化） |
| [porting-article.md](porting-article.md) | 上記を読み物としてまとめた記事原稿 |
| [dc-development-guide.md](dc-development-guide.md) | 起動する CDI を作る手順と、黒画面・砂嵐などの失敗時チェックリスト |
| [roadmap.md](roadmap.md) | 完成までのロードマップと進捗、QA の記録 |

These are development notes in Japanese, kept as written. Paths refer to the
private working tree, and the parts about the voice codec and the music source
are superseded by [formats.md](../formats.md).
