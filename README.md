# EVE-DC Engine

セガサターン版『EVE burst error』を Dreamcast で動かすための、非公式の再実装エンジンと変換ツールです。
An unofficial engine re-implementation and data converter that runs the Sega Saturn
version of *EVE burst error* on the Dreamcast. ([English](#english) below.)

> **このリポジトリにゲームのデータは一切含まれていません。**
> 遊ぶには、ご自身で所有しているサターン版『EVE burst error』のディスク 3 枚（ゲームディスク）が必要です。
> ディスクイメージや変換済みデータの配布・入手先の案内は行いません。Issue などへの添付もご遠慮ください。

本プロジェクトは個人による非公式のファン活動で、原作の権利者およびセガとは一切関係ありません。
作品名や機種名は各権利者の商標または登録商標です。

## 何ができるか

- サターン版のシナリオ（`.SCO`）をそのまま解釈する VM（台詞、メニュー、分岐、フラグ、ルート切替、ディスク交換）
- 背景・表情差分・ムービー・音声・効果音・BGM の再生
- VMU へのセーブ／ロード、バックログ、オート送り、既読スキップ
- サターン版と同じ 3 枚組（Disc1 小次郎編前半 / Disc2 まりな編前半 / Disc3 両編後半＋解決編）

開発中のため、原作と見た目や挙動が異なる箇所があります（「既知の制限」を参照）。

## 必要なもの

| 用途 | もの |
|---|---|
| ゲームデータ | 自分で吸い出したサターン版ディスク 3 枚のイメージ（`.cue` ＋ `.bin`、または 2048 バイト/セクタの `.iso`） |
| 変換 | Python 3.8 以降、`numpy`、PATH の通った `ffmpeg` |
| ビルド | [KallistiOS](https://github.com/KallistiOS/KallistiOS)（kos-ports の `libmpeg` を含む）。Windows では [DreamSDK](https://dreamsdk.org/) が手軽です |
| CD イメージ作成 | `mkisofs` と `cdi4dc`（DreamSDK に同梱） |
| 実行 | [Flycast](https://github.com/flyinghead/flycast) などのエミュレータ、または CD-R と実機 |

## 使い方

全体の流れは「ディスクを吸い出す → 変換する → ビルドする → 起動する」の 4 段階です。
以下のコマンドは、このリポジトリを取得したフォルダの直下で実行します。

### 0. 準備

1. **ディスクイメージを用意する。** 手持ちのサターン版ゲームディスク 3 枚（小次郎ディスク、まりなディスク、
   3 枚目のディスク）を、お使いの吸い出しソフトで `.cue` ＋ `.bin` 形式にします。
   メイキングディスク（4 枚目）は使いません。渡しても無視されます。
2. **Python と numpy を入れる。** Python 3.8 以降を入れたうえで、次を実行します。

   ```bash
   python -m pip install numpy
   ```

3. **ffmpeg を入れる。** `ffmpeg -version` が動けば準備完了です（PATH を通してください）。
4. **KallistiOS を入れる。** Windows では DreamSDK を入れるのが簡単です。`libmpeg`（kos-ports）も必要です。

### 1. 変換する

ディスクイメージ 3 枚を渡します。順番は自由で、どれが何枚目かはイメージの中身から自動で判定します。
`.cue` の代わりに、データトラックの `.bin`（「Track 1」のファイル）や `.iso` を直接渡しても構いません。

```bash
python tools/convert.py "Disc 1.cue" "Disc 2.cue" "Disc 3.cue"
```

実行すると、ディスクごとに次のような進行状況が出ます。

```
disc 1: ... (Disc 1) (Track 1).bin
== Disc 1 -> cd_root\Disc1
  scenario: 90 files
  graphics: 228 pictures for 29 scripts
  movies: 17
  voices: 4952
  bgm: 46 songs
  total 262.8 MiB
```

- 出力先は `cd_root/Disc1`、`Disc2`、`Disc3` です。合計で約 1.1GB になります。
- 音声が約 2 万本あるため、初回は 20〜40 分ほどかかります（CPU のコア数によります）。
- 途中で止めても、もう一度同じコマンドを実行すれば続きから再開します。
- 最後に `not converted` と出た場合は、その行に変換できなかったファイル名が並びます。
  3 枚すべてを渡していないと、別のディスクにある画像やムービーが不足します。

主なオプション:

| オプション | 意味 |
|---|---|
| `--out フォルダ` | 出力先を変える（既定は `cd_root`。変えた場合は `make cdi CDROOT=フォルダ/Disc1` のように指定） |
| `--only-disc 2` | そのディスクの分だけ作る（イメージは 3 枚とも渡すのがおすすめ） |
| `--skip voices,bgm` | 指定した工程を飛ばす（`scenario`、`graphics`、`movies`、`voices`、`bgm`） |
| `--jobs 4` | 並列数を指定する（既定は CPU のコア数） |
| `--force` | すでにあるファイルも作り直す |

### 2. ビルドする

KallistiOS の環境が有効なシェル（DreamSDK なら「DreamSDK Shell」）で、ディスクごとに実行します。

```bash
make cdi DISC=1
```

```bash
make cdi DISC=2
```

```bash
make cdi DISC=3
```

`build/eve_dc_disc1.cdi`、`eve_dc_disc2.cdi`、`eve_dc_disc3.cdi` ができます。
エンジンはディスク番号ごとにコンパイルし直すので、3 枚とも `make cdi` が必要です。

- デバッグ出力（シリアルログ）の無い版は `make release DISC=1` で作ります。実機で遊ぶ場合はこちらを使ってください。
- 起動時のロゴとオープニングを省くには `make cdi DISC=1 EXTRA_CFLAGS="-DNO_INTRO"` とします。
- 最初からやり直すときは `make clean` を実行します（`cd_root` は消えません）。

### 3. 起動する

- **エミュレータ**: Flycast などで `build/eve_dc_disc1.cdi` を開きます。
- **実機**: CDI 形式に対応した書き込みソフトで CD-R に焼きます。3 枚とも 80 分 CD-R に収まります。
- ゲームは必ずディスク 1 から始めます。物語が進むと「ディスク n に交換してください」と表示されるので、
  該当する CDI（または CD-R）に入れ替えると続きが始まります。
- セーブにはビジュアルメモリ（VMU）が必要です。エミュレータでは VMU を有効にしておいてください。

### うまくいかないとき

| 症状 | 対処 |
|---|---|
| `numpy is required` | `python -m pip install numpy` を実行する |
| `ffmpeg was not found on PATH` | ffmpeg を入れて PATH を通す |
| `skipping ...: not a game disc` | ゲームディスク以外（メイキングディスクや音声トラック）を渡している。データトラックのイメージを指定する |
| `no ISO9660 volume descriptor` | `.cue` か「Track 1」の `.bin` を指定する（「Track 2」は音声トラック） |
| `cd_root/Disc1 not found` | 先に `tools/convert.py` を実行する |
| `kos-cc` が見つからない、`makeip` がエラーで止まる | DreamSDK Shell など、KallistiOS の環境が有効なシェルで `make` を実行する |
| 音声や BGM が鳴らない | 変換のログに `not converted` が無いか確認し、`--force` を付けずに再実行して不足分を作る |

### 操作

| ボタン | 動作 |
|---|---|
| A | 送る／決定 |
| B | キャンセル、長押しで早送り |
| 十字キー | カーソル移動 |
| Start | システムメニュー（セーブ、ロード、キャラクター切替、オート送り、既読スキップ、タイトルへ） |
| X | バックログ |
| Y | オート送りの切替 |

## 構成

```
main.c, src/, include/   エンジン本体（C、KallistiOS）
src/adx/                 libADX の改変版（ライセンスはファイル冒頭に記載）
tools/convert.py         ディスクイメージ → cd_root/ の一括変換
tools/satdisc.py         サターンのディスクイメージ（ISO9660）読み出し
tools/sco.py             .SCO シナリオの逆アセンブラ
tools/b1.py, cmpdec.py   画像（.B1）と圧縮データ（.CMP）のデコーダ
tools/satvoice.py        音声（6 ビット対数 DPCM）のデコーダ
tools/satseq.py, satbgm.py  BGM（SCSP シーケンス＋音色バンク）のパーサとソフトシンセ
docs/formats.md          解析したファイル形式の説明
```

## 既知の制限

- BGM はサターンのシーケンスデータをソフトシンセで再現したもので、実機の音と完全には一致しません
  （FM 音色、DSP リバーブ、ピッチエンベロープは未対応）。
- パレットアニメーションなど一部の画面効果は近似です。
- タイトル画面やセーブ画面の見た目は原作と異なります。
- 実機での動作確認は十分ではありません。
- 小次郎編 A001_2 の 1 場面（「？？？？？！！！！！」）で鳴るはずの BGM 52 は無音です。
  シナリオ命令 `8026 52` に対応する曲がサターン版の曲バンク（GAMEA〜D / JOIN / DIARY、1〜47 番）に存在せず、
  Windows 版ではこの命令だけ効果音 `eve_52.ldp`（未解読の独自圧縮）に置き換えられているためです。
  影響は 1 回の短いジングルのみです。

## 参考資料

このリポジトリ内の文書:

- [docs/formats.md](docs/formats.md) — 解析したファイル形式（シナリオ、画像、圧縮、音声、BGM）
- [docs/reference/dreamcast-homebrew-knowledge.md](docs/reference/dreamcast-homebrew-knowledge.md) — Dreamcast ホームブリュー開発の知見まとめ
- [docs/reference/porting-article.md](docs/reference/porting-article.md) — 移植で踏んだ落とし穴をまとめた記事原稿
- [docs/reference/dc-development-guide.md](docs/reference/dc-development-guide.md) — CDI 作成手順と失敗時のチェックリスト
- [docs/reference/roadmap.md](docs/reference/roadmap.md) — 完成までのロードマップと進捗

`docs/reference/` は開発中の作業ツリーで書いた記録で、パスや一部の記述は公開版と異なります
（[注意書き](docs/reference/README.md)を参照）。

外部の資料:

- [KallistiOS](https://github.com/KallistiOS/KallistiOS) と [kos-ports](https://github.com/KallistiOS/kos-ports)（libADX、libmpeg）
- [DreamSDK](https://dreamsdk.org/)
- [Flycast](https://github.com/flyinghead/flycast)
- セガサターンのサウンドドライバ／トーンエディタのマニュアル（シーケンスと音色バンクの形式）
- [VGMTrans](https://github.com/vgmtrans/vgmtrans)（サターンのシーケンスイベントの仕様確認）
- SCSP ユーザーズマニュアル（エンベロープと LFO のレート表）

## ライセンスと権利について

- 本リポジトリのソースコードとドキュメントは [MIT License](LICENSE) です。
- `src/adx/` は libADX（BSD 系ライセンス）の改変版です。詳細は [THIRD_PARTY.md](THIRD_PARTY.md) を参照してください。
- ファイル形式の情報は、正規に入手したディスクの解析（相互運用性の確保を目的としたもの）によって得ています。
  原作の実行コード、シナリオ、画像、音声、音楽は含みません。
- 変換で生成されたファイル（`cd_root/`、`build/`）には原作の著作物が含まれます。
  個人で楽しむ範囲にとどめ、再配布しないでください。`.gitignore` で除外してあります。

---

## English

An unofficial, from-scratch engine that plays the Sega Saturn version of
*EVE burst error* (the 1997 release) on the Sega Dreamcast, together with the tools
that convert the data of your own discs.

> **This repository contains no game data.** You need the three game discs of the
> Saturn release, dumped by yourself. Do not ask for, link to, or attach disc
> images or converted files.

This is a fan project. It is not affiliated with or endorsed by the rights holders
of the game or by SEGA. All trademarks belong to their respective owners.

### Requirements

- Images of your three Saturn discs (`.cue` + `.bin`, or a 2048-byte `.iso`)
- Python 3.8+, `numpy`, and `ffmpeg` on `PATH`
- [KallistiOS](https://github.com/KallistiOS/KallistiOS) with the `libmpeg` port
  ([DreamSDK](https://dreamsdk.org/) on Windows), plus `mkisofs` and `cdi4dc`

### Build

```bash
python -m pip install numpy
```

```bash
python tools/convert.py "Disc 1.cue" "Disc 2.cue" "Disc 3.cue"
```

```bash
make cdi DISC=1
```

1. **Convert.** Pass the three game discs in any order (a `.cue`, the "Track 1"
   `.bin`, or an `.iso`; the bonus fourth disc is ignored). `convert.py` writes
   `cd_root/Disc1..3`, about 1.1 GB in total. The first run takes 20 to 40 minutes
   because there are about 20,000 voice clips; an interrupted run resumes where
   it stopped. Items listed after `not converted` could not be made, usually
   because one of the three discs was not given.
   Options: `--out DIR`, `--only-disc N`, `--skip voices,bgm,...`, `--jobs N`, `--force`.
2. **Build.** In a shell with the KallistiOS environment (the DreamSDK Shell on
   Windows) run `make cdi DISC=1`, then `DISC=2` and `DISC=3`: the engine is
   compiled per disc. This gives `build/eve_dc_disc<n>.cdi`. Use
   `make release DISC=n` for images without the serial debug log, and
   `EXTRA_CFLAGS="-DNO_INTRO"` to skip the logo and opening movies.
3. **Run.** Open `build/eve_dc_disc1.cdi` in an emulator such as Flycast, or burn
   the images to 80-minute CD-Rs. Always start with disc 1; the game asks for
   the other discs as the story goes on, exactly like the Saturn release.
   Saving needs a VMU.

If `make` cannot find `kos-cc` or `makeip` stops with an error, you are not in
the KallistiOS shell. If `convert.py` says an image is "not a game disc" or has
"no ISO9660 volume descriptor", pass the data track ("Track 1") or the `.cue`.

Controls: A advance / confirm, B cancel (hold to fast-forward), D-pad cursor,
Start system menu (save, load, switch character, auto, skip read text, title),
X backlog, Y auto mode.

### What is implemented

The scenario VM (text, menus, branches, flags, route switching, disc changes),
backgrounds and overlays, movies, voices and sound effects, music, VMU saves,
backlog, auto mode and skipping. The file formats are described in
[docs/formats.md](docs/formats.md).

Known limits: the music is rendered by a software synthesiser from the Saturn
sequence data and does not match the console exactly (no FM voices, DSP reverb
or pitch envelopes); some screen effects are approximations; the title and save
screens look different; testing on real hardware is limited.
BGM 52, cued once in the Kojiro route (A001_2, the "?????!!!!!" sting), stays
silent: the Saturn song banks (GAMEA-D / JOIN / DIARY, songs 1-47) have no song
52, and the Windows version replaces that one instruction with the sound
effect eve_52.ldp, whose codec is not decoded. Only that short jingle is lost.

### References

File formats: [docs/formats.md](docs/formats.md). Development notes (Japanese):
[docs/reference/](docs/reference/README.md). External: KallistiOS and kos-ports,
DreamSDK, Flycast, Sega's Saturn sound driver manuals, VGMTrans, the SCSP
user's manual (see [THIRD_PARTY.md](THIRD_PARTY.md)).

### License

Source code and documentation: [MIT](LICENSE). `src/adx/` is a modified copy of
libADX under its own BSD-style license, see [THIRD_PARTY.md](THIRD_PARTY.md).
Files produced by the converter (`cd_root/`, `build/`) contain copyrighted
material of the original game: keep them for personal use and do not
redistribute them.
