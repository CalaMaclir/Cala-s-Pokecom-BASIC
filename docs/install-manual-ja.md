# Cala's Pokecom BASIC System
## Version 0.91
## Install Manual / 導入マニュアル

本書は、ClockworkPi PicoCalc と Raspberry Pi Pico 2 W に Cala's Pokecom BASIC（CPB）v0.91 を導入・更新するための手順書です。

## 1. 対応ハードウェア

- ClockworkPi PicoCalc
- Raspberry Pi Pico 2 W（RP2350、無線機能付き）
- FAT32 の SD カード（推奨）
- 初回書き込み用のデータ通信対応 Micro-USB ケーブル

Pico、Pico W、Pico 2（非W）は v0.91 の配布対象ではありません。PicoCalc の PSRAM は任意機能です。搭載・検出時はINTERNAL Program Storageが1,024行×2,047文字になり、Editor Undo / Redo、SD cache、DirectState、Compiled Program cacheにも利用されます。PSRAMが利用できない場合もINTERNALは256行×191文字のSRAM fallbackで動作します。

## 2. 配布物

GitHub Actions の artifact `CPokecombasic-v0.91-build<run>-pico2w` を展開し、通常版の `build/CPokecombasic.uf2` を使用します。

`CPokecombasic-editor-perf.uf2` が同梱される場合、それはエディタ性能測定用です。通常利用には使用しないでください。

正式配布ではGitHub Actions artifact ZIPを完全パッケージとして使用します。Windows用`flash-cpb.cmd`はこのZIP内の`build\CPokecombasic.uf2`を参照するため、`flash-cpb.cmd`だけを単独で取り出して配布・運用しないでください。

SD カードへ置く主なファイルは次のとおりです。

| ファイル | 用途 |
|---|---|
| `RMBASIC.CFG` | 表示、音量、RTC、無線、Quick Load Keys などの設定 |
| `STARTUP.WAV` | 起動音。設定で有効にした場合だけ再生 |
| `*.BAS` | BASIC プログラム |
| `*.WAV` / `*.MP3` | Files または `WAVPLAY` から再生する音声 |

## 3. 初回インストール

1. PicoCalc の電源を切ります。
2. Pico 2 W 側の Micro-USB ケーブルを外します。
3. Pico 2 W の BOOTSEL ボタンを押したまま Micro-USB を接続します。
4. PC に `RPI-RP2` ドライブが現れたら BOOTSEL を離します。
5. `CPokecombasic.uf2` を `RPI-RP2` へコピーします。
6. 自動的に再起動した後、PicoCalc の LCD に CPB の起動画面が表示されることを確認します。

コピー後に `RPI-RP2` が切断されるのは正常です。

## 4. SDカードの準備

1. SD カードを FAT32 で初期化します。
2. 必要な `*.BAS`、音声、`RMBASIC.CFG` をルートへコピーします。
3. PicoCalc の電源を切ってから SD カードを挿入します。
4. 起動後、`Control Center → System Information` で `SD PRESENT / MOUNTED` を確認します。

v0.91 の Files はルートディレクトリのみを表示します。ファイル名には内部の ASCII 空白を使用できますが、先頭・末尾の空白やドット、`..`、`/`、`\\`、制御文字、非ASCII文字は使用できません。最大長は79文字です。

## 5. 初回起動の確認

空の `BASIC>` プロンプトで HOME（Shift+Tab）を押して Control Center を開きます。

確認項目:

- `System Information` に `v0.91` と表示される
- CPU clock が安全な既定値 `150 MHz` で起動する
- SD を使用する場合は `PRESENT / MOUNTED`
- PSRAM 搭載機では容量、PIO、クロック、Program Store／Direct State／Compiled Cache等のruntime allocationが表示される
- INTERNAL RAMを選んだ場合、PSRAM利用時はcapacityが1,024 lines / 2,047 charsになる
- Wi-Fi は起動時 `OFF`（session-only）

## 6. v0.89以前からの更新

プログラムと設定をバックアップしてから更新してください。通常は SD カード上の `*.BAS` と `RMBASIC.CFG` をそのまま引き継げます。

v0.91 の主な互換上の注意:

- `WAVPLAY` は WAV に加えて MP3 を内容判定して再生します。
- Audio 設定に `WAV/MP3` と `PLAY` の独立音量が追加されています。
- CPU profile は再起動すると必ず 150 MHz に戻ります。
- Files と Full-Screen Editor で内部空白を含むファイル名を扱えます。
- Full-Screen Editor の Undo / Redo 履歴は session-only で、エディタ終了時に破棄されます。
- INTERNAL Program Storage はPSRAM利用時に1,024行×2,047文字へ拡張されます。PSRAMなしでは256行×191文字です。
- USB CDC／UART file transferはbulk化され、UARTは115200／230400／460800／921600 bpsと任意RX DMAを選択できます。
- `LIST`はBREAK／Esc／Ctrl-Cで途中停止できます。`LOAD`時は`LOADING... (filename)`、`RUN`開始時は`RUN...`を表示します。

## 7. 本体メニューからの更新

対応版が既に動作している場合は、`Control Center → Firmware → Enter BOOTSEL` を選ぶと物理 BOOTSEL 操作なしで `RPI-RP2` に入れます。その後 `CPokecombasic.uf2` をコピーします。

古いファームウェアから初めて対応版へ移行する場合は、物理 BOOTSEL を使ってください。

## 8. Windowsのflash-cpb.cmd

`picotool.exe` を PATH に置き、配布物の `flash-cpb.cmd` を実行します。スクリプトは接続確認、BOOTSEL への移行、UF2 書き込みを補助します。

実行前に:

- PicoCalc の USB Storage を停止する
- シリアルターミナルを閉じる
- Pico 2 W 側 USB を PC へ接続する
- 正しい `CPokecombasic.uf2` が同じ作業場所にあることを確認する

## 9. リカバリ

起動しない、画面が乱れる、更新が途中で止まった場合は次を行います。

1. 電源を切り、SD カードを外します。
2. 物理 BOOTSEL で `RPI-RP2` を開きます。
3. 通常版 `CPokecombasic.uf2` を再書き込みします。
4. SD なしで起動を確認します。
5. 必要なら `RMBASIC.CFG` を退避して既定設定で起動します。

## 10. トラブルシューティング

### RPI-RP2が見えない

- 充電専用ではなくデータ通信対応ケーブルを使う
- BOOTSEL を押したまま接続する
- USB ハブを避けて PC へ直接接続する
- 別ポートまたは別ケーブルを試す

### SD NOT AVAILABLEと表示される

- FAT32 で再初期化する
- 電源を切って差し直す
- `Control Center → SD Card` で状態を確認する
- 別の SD カードを試す

### PSRAMがNOT AVAILABLEになる

通常機能はSRAM fallbackで継続できます。電源を入れ直し、装着状態を確認してください。診断はPSRAM内容を上書きするため、INTERNAL ProgramStoreやDirectStateなどpersistent clientが使用中の場合は安全のため拒否されます。必要なprogramを先にSDへ保存してください。

### 更新後に設定が不自然

`RMBASIC.CFG` を退避して起動し、既定設定で再現するか確認します。CPU、Wi-Fi、Bluetooth 接続、エディタ履歴は session-only です。

---

操作全般は `system-manual-ja.md`、BASIC 言語は `programming-reference-ja.md`、v0.91 の変更点は `release/v0.91-release-notes.md` を参照してください。
