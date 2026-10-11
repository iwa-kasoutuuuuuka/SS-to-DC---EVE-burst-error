# 🎮 Dreamcast Homebrew 開発ガイド (KOS版)

このガイドでは、Sega Dreamcast向けに KallistiOS (KOS) を使用して、確実に起動するCDIイメージを作成する手順を解説します。

## 1. ディレクトリ構成
プロジェクトのルートディレクトリ（`E:\DC_EVE`）を以下の構成にします。

```text
/E:/DC_EVE
 ├─ main.c           (ソースコード)
 ├─ Makefile         (ビルド設定)
 ├─ engine/
 │   └─ IP.BIN       (ブートセクタ header)
 ├─ iso_root/        (CD作成用一時フォルダ)
 │   └─ 1ST_READ.BIN (生成されたブートバイナリ)
 └─ build/           (最終出力 CDI の保存先)
```

## 2. ソースコード (main.c)
主要な初期化処理、無限ループ、文字表示、コントローラ入力を実装しています。
[main.c](file:///e:/DC_EVE/main.c)

## 3. Makefile
KOS環境依存の設定を含むビルドファイルです。`make cdi` で一気にイメージ作成まで行います。
[Makefile](file:///e:/DC_EVE/Makefile)

## 4. ビルド手順（コマンド）

DreamSDK または KOS環境が整ったターミナルで以下を実行します。

### Step 1: ELFファイルの作成
```powershell
make clean
make
```
これにより `main.elf` が生成されます。

### Step 2: CDIイメージの作成
```powershell
make cdi
```
このコマンドは以下の処理を自動で行います：
1. `main.elf` をバイナリ形式に変換 (`1ST_READ.BIN`)
2. バイナリのスクランブル処理 (`scramble.exe`)
3. ISOイメージの作成 (`mkisofs.exe`)
4. CDIイメージへの変換 (`cdi4dc.exe`)

## 5. CDI/GDIイメージ作成の詳細手順

もし手動で実行したい場合は以下の流れになります：

1. **Scramble**: Dreamcast実機のCDブートには、バイナリを特定の順序で並び替える「スクランブル」が必要です。
   - `scramble 1ST_READ.BIN iso_root/1ST_READ.BIN`
2. **ISO Mastering**: IP.BIN をシステム領域に書き込みつつ ISO を作ります。
   - `mkisofs -V "DC_APP" -G engine/IP.BIN -r -J -l -o game.iso iso_root`
3. **CDI Conversion**: エミュレータや焼き込みソフトが認識できる形式にします。
   - `cdi4dc game.iso game.cdi`

## 6. 失敗時のチェックリスト (Black Screen 対策)

### 🔴 画面が真っ暗（起動しない）
- **原因 1: Scramble 漏れ**
  - 未スクランブルの `1ST_READ.BIN` は実機/Redream等で起動しません。Makefileで `scramble` が通っているか確認してください。
- **原因 2: IP.BIN の Boot Filename 不一致**
  - `IP.BIN` 内に記述されたファイル名が `1ST_READ.BIN` でない場合、読み込まれません。`IP.txt` を確認してください。
- **原因 3: LBA 矛盾**
  - 2セッション構成で作成する場合、LBA 11702 等のオフセット調整が必要です。本構成（単一セッションデータ）では `mkisofs` の `-G` オプションが正しければ動作します。

### 🔴 文字が表示されない
- **原因 1: `vid_init()` 忘れ**
  - 画面描画の基本クロックが動きません。
- **原因 2: `vid_clear()` または `pvr_scene_begin()` の順序**
  - 描画バッファをクリアせずに描画すると、ゴミデータで上書きされることがあります。

### 🔴 コントローラが効かない
- **原因: `maple_enum_type()` の戻り値 NULL チェック**
  - コントローラが刺さっていない、または初期化完了前にアクセスするとクラッシュします。

### 🔴 PVRテクスチャ描画が「砂嵐（サンドストーム）ノイズ」になる
PVRへ画像転送した際、画面がランダムなモザイクや市松模様のノイズになる場合、以下の2つの深刻なバグが疑われます。

1. **VRAMアドレスのマップ指定漏れ**
   - **症状**: 何を描画しても背景やフォントがランダムなノイズの塊に変貌する。
   - **原因**: `sq_cpy` 等でVRAMへDMA/Store Queue転送する際、宛先ポインタがシステムメモリへのオフセット（例: `0x01234567`）になっており、物理アドレス空間を指していない。
   - **解決策**: 書き込み先ポインタのベースに `0xa4000000`（PVR_VRAM_P0）を付与する。
     ```c
     sq_cpy((void*)(0xa4000000 | (uintptr_t)vram_ptr), src, size);
     ```
2. **描画フォーマット（RGB/ARGB）の不一致**
   - **症状**: 形やシルエットはうっすら見えるが、映像が黒く欠けたり、色が毒々しく反転する。
   - **原因**: 読み込んだテクスチャが `RGB565`（透過なし16bpp）であるのに、描画コンテキスト (`pvr_poly_cxt_txr`) で `PVR_TXRFMT_ARGB1555`（透過あり）等を指定している。最上位ビットが「透明度フラグ」として誤認されている。
   - **解決策**: データと描画フラグを完全に一致させる。
     ```c
     pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE, width, height, txr, PVR_FILTER_NONE);
     ```

### 🔴 非正方形（長方形）テクスチャが崩れる
- **原因: PVR特有のTwiddle仕様（Rectangular Twiddle）**
- **解決策**: 512x256などの長方形テクスチャは、PVRハードウェアにTwiddledとして解釈させると高確率で表示崩れを起こす。PC側でテクスチャを **線形（Linear）バイナリ** に変換するか、PVR読込時にヘッダーを飛ばしてストライド転送（`PVR_TXRFMT_X32_STRIDE` と `pvr_txr_set_stride(幅)`）を行うのが最も堅牢で確実な手法である。

## 7. 現在のビルド・検証・アセット再生成（2026-10-10 時点）

### ビルド
```bash
make                      # デバッグビルド（シリアルにログ、自動走破はこれを使う）
make cdi                  # build/eve_dc.cdi（iso_root/ + iso_stage/audio/ を graft）
make release              # clean → -DNDEBUG で全リビルド → CDI。シリアル出力はエラーのみ
make EXTRA_CFLAGS="-DSTART_SCRIPT_NAME=B001"          # 任意の .SCO から起動（ロゴ／OP は飛ばす）
make EXTRA_CFLAGS="-DDEMO_AUTO_ADVANCE=4 -DDEMO_RANDOM=7 -DDEMO_KEYS=120 -DDEMO_SWITCH=300"   # 無人テスト
make cdi DISC=1 EXTRA_CFLAGS="-DDEMO_AUTO_ADVANCE=4 -DDEMO_SAVE_SLOT=3"   # 15 秒後にスロット 3 へセーブ（VMU にディスク番号付き v3 セーブを作る）
make cdi DISC=2 EXTRA_CFLAGS="-DDEMO_AUTO_ADVANCE=4 -DDEMO_LOAD_SLOT=3"   # 15 秒後にスロット 3 をロード → 別ディスクのセーブなら「ディスク n に交換してください」(QA Q11)
```
- `make` は DreamSDK の環境変数（DREAMSDK_HOME, KOS_BASE …）を持つシェルでしか通らない。python の subprocess から呼ぶと kos-cc の DreamSDK ヘルパーが環境を見つけられない（`auto_run.sh` がビルドを bash 側で行う理由）
- 引用符入りの `-D` は kos-cc ラッパーが壊す。文字列は `-DSTART_SCRIPT_NAME=A001_6` のように識別子で渡し、C 側で文字列化する
- 別のシェルや別セッションと同時に `make cdi` を走らせると CDI が壊れる（175MB 程度の短いファイルになる）。走らせる前に `tasklist` で make / mkisofs / cdi4dc / flycast が居ないことを確認する

### 自動走破・ログ採取
```bash
bash .claude/skills/eve-dc-build-test/scripts/auto_run.sh --start A001 --seconds 300 --random 7 --extra "-DSHORT_TEXT_LOG"
```
build/autorun_<START>.{log,json} に到達スクリプト・メニュー数・行数・欠落ファイル・未知命令・範囲外・ヒープ推移（MEM: 行、約 60 秒ごと）が出る。詳細は `.claude/skills/eve-dc-build-test/SKILL.md`。
- Flycast は `Start-Process` 経由で起動しないと「Flycast Serial Output」窓が出ない。シリアルは `scripts/serial_capture.ps1` が CONOUT$ を直接読む
- デバッグビルドはテキスト 1 行ごとのログでシリアル律速（約 3 行/秒）。長時間走行は `-DSHORT_TEXT_LOG`

### アセット再生成（詳細は eve-dc-asset-pipeline スキル）
| 種別 | コマンド | 出力 |
|---|---|---|
| シナリオ | `extract_sco.py <Track1.bin> saturn_fixed/DiscN --dir SCENARIO` → `iso_root/scenario/DiscN/` にコピー | 各 90 本 |
| 背景・立ち絵 | `py -3 -I .claude/skills/eve-dc-asset-pipeline/scripts/convert_images.py saturn_fixed/Disc1/SCENARIO/*.SCO` | iso_root/graphics/bg, spr（EVB1） |
| 音声 | `python -I .claude/skills/eve-dc-asset-pipeline/scripts/stage_voices.py <SCO...> --disc 1 --clean [--rate 22050]` | iso_stage/audio/Disc1 |
| ムービー | `convert_movies.py`（ffmpeg） | iso_root/movies/*.mpg |
| BGM | `convert_bgm.py --all saturn_fixed/Win95/MIDI` | iso_root/bgm/BGM<n>.wav |
| 検証 | `python -I .claude/skills/eve-sco-scenario-format/scripts/verify_sco.py saturn_fixed/Disc1/SCENARIO` | 命令境界・参照 100% を確認 |

### 失敗時のチェックリスト（追加分）
- **タイトルが真っ黒**: A01 は FADEOUT 0 の直後にメニューを出す。フェードの黒ポリゴンは z=4（背景の上・メッセージ窓の下）でなければ文字が隠れる
- **`LibADX: Stopping` の直後に固まる**: kos-ports 版 libADX の adx_stop() は音声ストリーミング中に CD を読むと戻らない。`src/adx/` の改変版を使い `-lADX` をリンクしないこと
- **ムービー後に音が出ない／次の音声で落ちる**: `video_play()` が終了後にサウンドドライバを再初期化しているか（snd_stream_shutdown → snd_shutdown → snd_init → snd_stream_init）
- **BGM で約 20 秒後に SH4 例外**: AICA は 64K サンプル超をハードウェアループできない。snd_sfx ではなく snd_stream（src/bgm.c）で鳴らす
- **メニューから抜けられない／セーブ項目が出ない**: 連続する 8002 は 1 画面に統合し、8006 は最上位のコマンドメニューへ戻す（vm.c の menu_stack）
- **セーブできない**: VMU が A1/A2 に刺さっているか（Flycast は既定で 2 枚）。ファイル名 EVEBE_01〜03、約 8.3KB