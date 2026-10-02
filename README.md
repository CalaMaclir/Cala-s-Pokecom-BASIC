# Cala's Pokecom BASIC Version 0.92

**for ClockworkPi PicoCalc + Raspberry Pi Pico 2 W**

Cala's Pokecom BASIC（CPB）は、PicoCalcだけでプログラムの作成・実行・保存、グラフィック、音楽、通信を行えるスタンドアロンBASIC環境です。現時点の対象は **Raspberry Pi Pico 2 W専用**です。

Copyright (C) 2026 Cala Maclir

[English README](README.en.md)

## 導入して最初のプログラムを動かす

1. [公開Releases](https://github.com/CalaMaclir/Cala-s-Pokecom-BASIC/releases)からv0.92の完全ZIPを取得・展開します。
2. build/CPokecombasic.uf2をBOOTSELで書き込みます。Windows updaterはZIP内の配置を保ち、picotool.exeをPATHへ置いて実行します。
3. BASIC>で次を入力します。

```basic
10 PRINT "HELLO CPB"
20 END
RUN
```

4. SAVE "HELLO"でSDへ保存します。Structuredを作るにはControl Center → Editor → New Program → Structured BASICを選び、Editorで編集します。

```basic
PRINT SQUARE(12)
FUNCTION SQUARE(X)
    RETURN X*X
END FUNCTION
```

F1で保存、Escで戻ってRUNすると144を表示します。Editorの00001の番号は表示専用でBASには保存しません。Tabは4列ごとのspace挿入です。

## 主な機能

- 行番号なしStructured BASIC、block IF、数値／文字列FUNCTION、localと明示GLOBAL
- フォルダー管理、Filesの6文字属性欄と行全体配色、STANDBYのLCD消灯／復帰
- BASICの直接実行、行番号付きプログラム、配列、文字列、`GOSUB`、`ON GOTO/GOSUB`
- 320×320 LCDと仮想座標によるグラフィック、`PAINT`、PCG
- 3 voice MML、BEEP、PCM WAV／MP3のbackground再生
- AUTO／SD CARD／INTERNAL RAMを選べるProgram Storage
- PSRAM利用時はINTERNAL RAM／SD CARDとも最大1,024行・1行本文2,047文字。PSRAMなしのINTERNALは256行・191文字へ安全にfallback
- Storage Continuityによる編集中sessionの復元とtransactional save
- `BASIC>`のsession-only command history（Up／Down、入力中draft復元）
- Control CenterのPage Up／Page DownとPROGRAMS／DIRECTORY file management
- 独立Full-Screen BASIC Editor（visual wrap、Find、Goto、PSRAM Undo／Redo）
- PicoCalc PSRAMのmulti-client allocator、256 KiB Editor history、INTERNAL ProgramStore、SD cache、DirectState、Compiled Program cache、reliability／benchmark診断
- Bluetooth Classic／BLE HID Keyboard（JIS／US layout、再接続・切断・登録解除）
- USB CDC／UART0のbulk transfer、UART 115200～921600 bps、任意RX DMA、YMODEM bulk受信／packet coalesce
- USB CDC、UART0、USB Mass Storage、Wi-Fi HTTP File Server
- FirmwareメニューとWindows用`flash-cpb.cmd`
- External RTC／I2C、Wi-Fi／NTP、F1～F10、Alt+S screenshot

詳しい手順は用途別の日本語マニュアルを参照してください。

[日本語マニュアル一覧](docs/manual-ja.md)から原稿・PDF3冊を開けます。

- [導入マニュアル](docs/install-manual-ja.md) / [v0.92 PDF](docs/Cala-Pokecom-BASIC-v0.92-Install-Manual-ja.pdf)
- [システムマニュアル](docs/system-manual-ja.md) / [v0.92 PDF](docs/Cala-Pokecom-BASIC-v0.92-System-Manual-ja.pdf)
- [プログラミング・リファレンス](docs/programming-reference-ja.md) / [v0.92 PDF](docs/Cala-Pokecom-BASIC-v0.92-Programming-Reference-ja.pdf)
- [v0.92 リリースノート](docs/release/v0.92-release-notes.md)
- [v0.92 制限事項](docs/release/v0.92-known-limitations.md)

## 動作環境

- ClockworkPi PicoCalc
- Raspberry Pi Pico 2 W（RP2350）
- FAT32形式のSD card（推奨）
- Wi-Fi機能使用時は2.4 GHz WLAN

Pico、Pico W、Pico 2（非W）は現在の配布対象ではありません。

## インストール

GitHub Actions artifact **`CPokecombasic-v0.92-build<run>-pico2w`** を取得し、内部の **`build/CPokecombasic.uf2`** を使用します。

初回はPico 2 W側Micro-USBと物理BOOTSEL操作でUF2を書き込みます。対応版導入後は次も利用できます。

- `Control Center → System → Firmware → Enter BOOTSEL`
- Windowsで`picotool.exe`をPATHに置き、`flash-cpb.cmd`を実行

古いファームウェアにはreset vendor interfaceがないため、この更新方式を使える版への最初の移行だけは従来のBOOTSEL操作が必要です。詳しくは[firmware update workflow](docs/development/firmware-update.md)を参照してください。

## Program Storage

| Mode / backend | 最大行数 | 1行本文 | 用途 |
|---|---:|---:|---|
| INTERNAL RAM（PSRAM） | 1,024 | 2,047 characters | SD不要の大規模program、Full-Screen Editor |
| INTERNAL RAM（SRAM fallback） | 256 | 191 characters | PSRAM未搭載／利用不可時の互換mode |
| SD CARD | 1,024 | 2,047 characters | 永続保存、PC編集、転送 |
| AUTO | 選択backendに従う | 選択backendに従う | SD利用可ならSD、不可ならINTERNAL |

本文長には行番号とその後のspaceを含みません。PSRAMが利用できるPicoCalcでは、INTERNAL RAMもSD CARDと同じ1,024行・2,047文字です。INTERNALはcompact index＋2 KiB text slotで保持し、行の挿入・削除時に巨大な本文領域をshiftしません。PSRAMが利用できない場合だけ従来の256行・191文字SRAM backendへfallbackします。

PicoCalcのinteractive line editorとDirect modeの入力は191文字のままです。長いlogical lineはFull-Screen Editor、USB Storage、XMODEM／YMODEM、Wi-Fi File Server等から扱えます。SDの大規模programは内容を保持したままINTERNAL PSRAMへ切り替えられます。

`LIST`はBREAK／Esc／Ctrl-Cで途中停止できます。長い2,047文字行の出力中もbounded chunkごとに停止を確認します。

## Control Center

空の`BASIC>` promptでHOME（Shift+Tab）を押します。v0.91ではカテゴリを見出しとしてまとめ、画面はrow 37まで使用し、row 38を空行、row 39をFunction Key表示として使います。

```text
Files
Editor
    New Program
Program
    Save Program
    Save Program As
    Quick Load Keys
    Program Storage
Storage
    SD Card
    USB Storage
Display & Audio
    Display
    Audio
Network
    Wireless LAN
    Wi-Fi File Server
    Bluetooth
Serial
    Console
    Serial Config
    File Transfer
System
    Date / Time
    Power / CPU
    Board LED
    Firmware
Diagnostics
    System Information
    PSRAM Diagnostics
Exit
```

メニューlabelから`...`は除去しています。Up／Downが1項目移動、Shift+Up／Shift+Downが1画面移動です。FilesはLeft／RightでPROGRAMSとDIRECTORYを切り替えます。PROGRAMSではLoad／Run、両modeではRename、Delete（確認あり）、File Info、Refreshを利用できます。

`BASIC>`で`EDIT`、Control Centerの`Editor`、またはFiles PROGRAMSの`E EDIT`からFull-Screen Editorを起動できます。F1 Save、F2 Find、F3 Next、F4 Undo、F5 Insert Line、F6 Save As、F7 Goto、F8 Previous、F9 Redo、F10 Delete Lineです。Escで終了します。PSRAM利用時のINTERNAL／SDはいずれも2,047文字logical lineを編集できます。

PSRAMは起動時に任意デバイスとして検出されます。v0.91ではEditor Historyだけでなく、INTERNAL ProgramStore、SD read cache、DirectState、Compiled Program cacheをmulti-client allocatorで共存させます。VMのopcode実行は内部SRAMで行い、PSRAMをhot execution memoryとして直接参照しません。

## Bluetooth Keyboardとfile transfer

Version 0.89以降のBluetoothはKeyboard入力専用です。Classic HID KeyboardとBLE HID Keyboardのpairing、reconnect、disconnect、forget、JIS／US layoutに対応します。Paired Devicesには`KEYBD C`または`KEYBD LE`として登録済みKeyboardだけを表示します。

File Transferでは`AUTO`、`USB CDC`、`UART0`を選べます。AUTOはcommandの入力元を尊重し、PicoCalc本体から開始した場合は接続済みUSB CDC、なければUART0を選びます。

v0.91ではtransfer hot pathをbulk化し、YMODEM RXのheader/payload/CRC exact-read、TX packet coalesce、USB CDC bulk read、UART ring bufferを導入しています。UARTは115200／230400／460800／921600 bpsから選択でき、RX-only RP2350 DMAも選択できます。実機では921600 bps・YMODEM DMAでPC→PicoCalc約33 KB/s、PicoCalc→PC約62 KB/sを確認しています。

- XMODEM：send／receive、single-file
- YMODEM：sendはsingle-file、receiveはsingle-file／multi-file batch
- YMODEM receiveはheader sizeを使い、末尾paddingを保存しません

## AudioとPCG

`PLAY`は最大3 voiceのMMLをbackground再生でき、v0.90 Stage 5では出力gainを改善しています。`WAVPLAY`はPCM WAVに加えてMP3（44.1/48 kHz、mono/stereo）をstreaming再生します。Control CenterのFiles一覧ではWAV/MP3を選んで`P`で再生・停止できます。MP3再生時は必要なCPU clockを自動選択し、停止後に元へ戻すため、事前の200 MHz手動切り替えは不要です。`GDEF`はprintable ASCIIに8×8 monoまたはindexed-color glyphを定義し、`GPRINT`で描画します。4 glyphを2×2に並べれば16×16のcomposite characterとして利用できます。

Control CenterのAudioには、全音声へ適用する`Master`、file再生用の`WAV/MP3`、MML専用の`PLAY`を独立して用意しています。WAV/MP3の基準出力はStage 5で従来の2倍に拡張しています。Key ClickにはOFF／SOFT／CLASSIC／SHARPがあり、物理PicoCalc keyのREPL／メニュー操作だけが対象です。

## Examples

| 分類 | 例 |
|---|---|
| Version 0.85 long-line／PCG／3 voice | `CPB_V085_LONG_LINE_MEGADEMO.BAS` |
| Audio／PCG／graphics | `CPB_BACH_MEGADEMO.BAS` |
| Graphics | `Lissajous_Gallery.BAS`、`mandel_graphics.bas`、`julia_graphics.bas` |
| 3D | `3dhat.bas`、`3dhat_mesh.bas` |
| Benchmark | `mandel_text.bas`、`picocalc_mand.bas` |

## 制限

FUNCTION 32、parameter 8、local 128（parameterを含む）、depth 16、文字列127文字、global 64。IL1536／string pool6144 bytesによりsource容量内でもcompile上限へ達します。大きいstring frameはdepth16前にOOMとなり得ます。PSRAMなしではUndo／Redoなし。SUB／labels／local配列／STATIC／OTA／native compilerは含みません。詳細はProgramming Referenceを参照してください。

## ビルド

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -G Ninja -DPICO_BOARD=pico2_w
cmake --build build
```

外向け成果物は`build/CPokecombasic.uf2`と`build/CPokecombasic.elf`です。内部CMake targetの`retrominibasic_picocalc`は互換性のため残しています。

## Version history

- v0.8：初公開。USB CDC + MSC、Program Storage、XMODEM／YMODEM、Wi-Fi File Server
- v0.81：PAUSE、INKEY、runtime input、BREAK応答性、current-file SAVE
- v0.82：External RTC／I2C、PCG、`&H` hexadecimal literal
- v0.83：3 voice MML、BEEP、WAV
- v0.84：Storage Continuity、dirty session recovery
- v0.85：Bluetooth Classic SPP／Console／file transfer、YMODEM batch receive、Firmware controls、`flash-cpb.cmd`、audio/display改善、Key Click、SD 2,047-character line、外向け`CPokecombasic`命名
- v0.87：Bluetooth Classic／BLE HID Keyboard
- v0.89：200 MHz Experimental、Board LED制御、BluetoothをKeyboard専用構成へ整理
- v0.90 Stage 1：Control Center page移動、`BASIC>` command history、Files管理拡張
- v0.90 Stage 2：独立Full-Screen BASIC Editor、長文編集、Find／Goto／安全なSave
- v0.90 Stage 3：visual wrap、任意PSRAM driver、reliability／benchmark診断
- v0.90 Stage 4：PSRAM 128 KiBを使うEditor Undo／Redo、session終了時解放
- v0.90 Stage 5：Files一覧からのWAV/MP3再生、`WAVPLAY` MP3拡張、MP3用CPU clock自動化、`PLAY`音量改善
- v0.90 Stage 6：Editor cursor fast path、SD viewport batch read、内部空白を含む安全なfile名
- v0.91 Stage 1：USB CDC／UART transfer bulk化、UART 921600 bps、任意RX DMA、Control Center再編
- v0.91 Stage 2：PSRAM multi-client、INTERNAL ProgramStore PSRAM化、64 KiB SD cache、VM arrayのlazy SRAM化
- v0.91 Stage 3：DirectState PSRAM、Compiled Program cache、INTERNAL 1024×2047、LIST中断、LOAD／RUN進捗表示

- v0.92：STANDBY LCD制御、Editor分割／連結、Directory管理、Structured BASIC／FUNCTION、Files UI、compiler／VM／cache改善

## 系譜と名称

CPBはCala MaclirのRetroMiniBASICを技術的基盤としてPicoCalc向けに設計したものです。repository名、内部target、namespace、`RMBASIC.*`互換file名には歴史的な名称が残りますが、現在の製品名は**Cala's Pokecom BASIC**です。

## License

本repositoryのlicense fileを参照してください。
