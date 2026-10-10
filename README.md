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

## 手順

```bash
python -m pip install numpy
```

1. ディスク 3 枚を変換します（順番は自由。ディスク番号はイメージから判定します）。

   ```bash
   python tools/convert.py "Disc 1.cue" "Disc 2.cue" "Disc 3.cue"
   ```

   `cd_root/Disc1`〜`Disc3` に Dreamcast 用のファイル一式ができます。音声が約 2 万本あるため、
   初回は数十分かかります。途中で止めても、再実行すれば続きから再開します。

2. KallistiOS の環境（DreamSDK Shell など）でビルドします。

   ```bash
   make cdi DISC=1
   ```

   `build/eve_dc_disc1.cdi` ができます。`DISC=2`、`DISC=3` も同様です。
   デバッグ出力の無い版は `make release DISC=1` です。

3. `build/eve_dc_disc1.cdi` をエミュレータで開くか、CD-R に焼いて実機で起動します。
   ゲームはディスク 1 から始まり、物語の進行に応じてディスク交換を求められます。

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

`convert.py` writes `cd_root/Disc1..3` (it takes a while: there are about 20,000
voice clips; an interrupted run resumes where it stopped). `make cdi DISC=n`
produces `build/eve_dc_disc<n>.cdi`. Boot disc 1; the game asks for the other
discs as the story goes on, exactly like the Saturn release.

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

### License

Source code and documentation: [MIT](LICENSE). `src/adx/` is a modified copy of
libADX under its own BSD-style license, see [THIRD_PARTY.md](THIRD_PARTY.md).
Files produced by the converter (`cd_root/`, `build/`) contain copyrighted
material of the original game: keep them for personal use and do not
redistribute them.
