# Cala's Pokecom BASIC

[日本語](#日本語) | [English](#english) | [GitHub Releases](https://github.com/CalaMaclir/Cala-s-Pokecom-BASIC/releases)

## 日本語

### 1. Cala's Pokecom BASICとは

**Cala's Pokecom BASIC**は、ClockworkPi PicoCalcとRaspberry Pi Pico 2 W（RP2350）向けの、単体で動作する行番号付きBASIC環境です。電源投入後にBASICが起動し、プログラムの作成・実行・保存、グラフィック、ファイル転送、Wi-FiによるSDカード管理をPicoCalc上で行えます。

### 2. Version 0.8 / First Public Release

**Version 0.8は、Cala's Pokecom BASICとして初の一般公開リリースです。** Git tagおよびGitHub Releaseでは`v0.8.0`と表記します。

### 3. 対応ハードウェア

- ClockworkPi PicoCalc
- Raspberry Pi Pico 2 W
- RP2350

Version 0.8の正式対応構成は、PicoCalcにPico 2 Wを搭載した構成です。Pico 2、Pico、Pico Wなど、その他のRaspberry Pi Picoシリーズは正式対応機種ではありません。

### 4. 主な機能

- 行番号付きプログラムと直接実行モード
- 数値・文字列変数、数値・文字列配列
- `IF / THEN / ELSE`、`FOR / NEXT / STEP`、`WHILE / WEND`、`DO / LOOP`
- `GOTO`、`GOSUB / RETURN`、`ON GOTO`、`ON GOSUB`
- SD-backedおよびRAM Program Storage
- USB CDC BASIC Console + USB Mass Storage
- YMODEM、XMODEM-CRC、Wi-Fi HTTP File Server
- 320×320グラフィック、スクリーンショット、Control Center
- ソフトウェア時計、NTP、外付けPCF8563 RTC、`STANDBY`

### 5. Download / GitHub Releases

正式な配布先は[GitHub Releases](https://github.com/CalaMaclir/Cala-s-Pokecom-BASIC/releases)です。一般利用者向けの正式配布にはGitHub Actions Artifactを使用しません。

1. GitHub Releasesを開きます。
2. `Cala-Pokecom-BASIC-v0.8-pico2w.uf2`を取得します。
3. Pico 2 W基板上のBOOTSELを使用してUF2を書き込みます。

### 6. インストール

1. Pico 2 WをPicoCalcから取り外します。
2. BOOTSELボタンを押したまま、**Pico 2 W基板上のMicro-USB**でPCへ接続します。
3. 表示されたRPI-RP2ドライブへUF2をコピーします。
4. Pico 2 WをPicoCalcへ戻し、FAT32形式のSDカードを挿入して起動します。

| 端子 | 用途 |
|---|---|
| Pico 2 W Micro-USB | BOOTSEL、UF2書き込み、USB CDC BASIC Console、USB Mass Storage |
| PicoCalc本体USB Type-C | 電源、充電、CH340C経由UART0 |

Version 0.8のUSB Mass StorageはPicoCalc本体USB Type-Cからは提供されません。

### 7. Program Storage

Program Storage modeは`AUTO`、`SD CARD`、`INTERNAL RAM`です。

- SD-backed Program Store：最大1,024行
- Internal RAM Program Store：最大256行

SD-backed Program Storeはソース本文をSDカード側に保持してRAM使用量を抑えます。USB Mass StorageからSDカードが返却された後は古いindexを再利用せず、backing BASを再scan・verifyします。

### 8. USB CDC + Mass Storage

Pico 2 W基板上のMicro-USBから、固定descriptorのUSB Composite DeviceとしてUSB CDC BASIC ConsoleとUSB Mass Storageを同時に提供します。Windows実機でSDカード認識、read、create、overwrite、delete、およびCDC Consoleとの共存を確認済みです。

SDカードはCPBファームウェアまたはUSBホストのどちらか一方だけが所有します。通常はホスト側でSafe Ejectした後、Control Centerで`Return SD to CPB`を選択します。`Force Disconnect`はCDCを維持したままMSC mediaだけを切断する緊急操作です。ホスト側に未送信write cacheがある場合はデータ損失の可能性があります。

### 9. YMODEM / XMODEM

通常のシリアルファイル転送にはYMODEMを推奨します。single-file transfer、filename transfer、exact file size、1 KiB data block、およびPC ↔ PicoCalcの双方向転送に対応し、Tera Termで実機往復確認済みです。複数ファイルbatch transferは未実装です。

XMODEM-CRCもPC ↔ PicoCalcで実機確認済みです。ただしファイル名と元サイズを通知しないため、末尾に`0x1A` paddingが残る場合があります。通常用途にはYMODEMまたはWi-Fi File Serverを推奨します。

### 10. Wi-Fi File Server

Pico 2 WのWi-Fiを利用し、同一LANのブラウザからSDカードのUpload、Download、Delete、Refreshができます。Wi-FiとFile Serverは起動時OFFで、利用者が明示的に開始します。

Wi-Fi passwordは現在、SDカード上の`RMBASIC.CFG`へ平文で保存されます。配布サンプルのSSIDとpasswordは空欄です。

### 11. BASIC言語機能

行番号付きプログラム、直接モード、数値・文字列変数、数値・文字列配列、条件分岐、各種ループ、サブルーチン、`ON GOTO`、`ON GOSUB`を利用できます。

### 12. Graphics

主な機能は`SCREEN`、`CLS`、`COLOR`、`COLORHSV`、`PSET`、`LINE`、`CIRCLE`、`BOX`、`PAINT`、`LOCATE`、`GLOCATE`、`GPRINT`、`SAVEIMAGE`、`FLUSH`、`SLEEP`です。320×320 LCD全体を利用できます。

`Alt+S`は320×320・24-bit BMPを`SCREEN0001.BMP`などの自動採番で保存します。REPLの`SCREENSHOT`およびBASICの`SAVEIMAGE`も利用できます。

### 13. Control Center / UI

- 3行固定ステータス
- F1～F10 Quick Load / Run
- HOME（Shift+Tab）Control Center
- battery、charging、date/time、Wi-Fi、Caps Lock、CPU profile、console routing、RUN time

CPU profileはFULL 150 MHz、NORMAL 100 MHz、ECO 75 MHzです。起動時はFULL 150 MHzです。

### 14. Build from Source

Pico SDK 2.3.1を使用します。

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -G Ninja -DPICO_BOARD=pico2_w
cmake --build build
```

内部のCMake targetおよび生成名は互換性のため`retrominibasic_picocalc`のままです。

### 15. Documentation

- [Version 0.8 日本語マニュアル](docs/manual-ja.md)
- [Version 0.8 日本語PDF](docs/Cala-Pokecom-BASIC-v0.8-ja.pdf)
- [Roadmap](ROADMAP.md)

### 16. Project Lineage

Cala's Pokecom BASICは、Cala Maclirが制作したRetroMiniBASICを技術的基盤として、同じCala Maclir本人がClockworkPi PicoCalc向けに設計・実装したものです。第三者による単なる移植ではありません。

互換性や歴史的理由から、内部コードや生成物に`RetroMiniBASIC`、`RMB`、`RMBASIC`、`retrominibasic_picocalc`などの名称が残っています。ユーザー向け正式名称は**Cala's Pokecom BASIC**です。

### 17. License

BSD 3-Clause Licenseです。詳細は[LICENSE](LICENSE)を参照してください。

---

## English

### 1. What is Cala's Pokecom BASIC?

**Cala's Pokecom BASIC** is a standalone, line-numbered BASIC environment for ClockworkPi PicoCalc with Raspberry Pi Pico 2 W (RP2350). It starts directly into BASIC and supports editing, running and saving programs, graphics, file transfer, and Wi-Fi SD-card management on the PicoCalc.

### 2. Version 0.8 / First Public Release

**Version 0.8 is the first public release of Cala's Pokecom BASIC.** The Git tag and GitHub Release use `v0.8.0`.

### 3. Supported Hardware

- ClockworkPi PicoCalc
- Raspberry Pi Pico 2 W
- RP2350

The officially supported Version 0.8 configuration is a PicoCalc fitted with a Pico 2 W. Raspberry Pi Pico 2, Pico, Pico W, and other Pico-series boards are not officially supported by this release.

### 4. Main Features

- Line-numbered programs and direct execution mode
- Numeric and string variables; numeric and string arrays
- `IF / THEN / ELSE`, `FOR / NEXT / STEP`, `WHILE / WEND`, `DO / LOOP`
- `GOTO`, `GOSUB / RETURN`, `ON GOTO`, `ON GOSUB`
- SD-backed and internal-RAM Program Storage
- USB CDC BASIC Console plus USB Mass Storage
- YMODEM, XMODEM-CRC, and Wi-Fi HTTP File Server
- 320×320 graphics, screenshots, and PicoCalc Control Center
- Software clock, NTP, optional PCF8563 RTC, and `STANDBY`

### 5. Download / GitHub Releases

[GitHub Releases](https://github.com/CalaMaclir/Cala-s-Pokecom-BASIC/releases) is the official distribution channel. GitHub Actions artifacts are not the official end-user download channel.

1. Open GitHub Releases.
2. Download `Cala-Pokecom-BASIC-v0.8-pico2w.uf2`.
3. Flash the UF2 to the Pico 2 W using BOOTSEL.

### 6. Installation

1. Remove the Pico 2 W from the PicoCalc.
2. Hold BOOTSEL and connect the PC to the **Micro-USB connector on the Pico 2 W board**.
3. Copy the UF2 to the RPI-RP2 drive.
4. Reinstall the Pico 2 W, insert a FAT32-formatted SD card, and start the PicoCalc.

| Connector | Purpose |
|---|---|
| Pico 2 W Micro-USB | BOOTSEL, UF2 flashing, USB CDC BASIC Console, USB Mass Storage |
| PicoCalc USB Type-C | Power, charging, and UART0 through CH340C |

Version 0.8 USB Mass Storage is not provided through the PicoCalc USB Type-C connector.

### 7. Program Storage

Program Storage modes are `AUTO`, `SD CARD`, and `INTERNAL RAM`.

- SD-backed Program Store: up to 1,024 lines
- Internal RAM Program Store: up to 256 lines

The SD-backed store keeps source text on the SD card to reduce RAM usage. After the SD card is returned from USB Mass Storage, CPB discards the old index and rescans and verifies the backing BAS file.

### 8. USB CDC + Mass Storage

The Pico 2 W Micro-USB connector exposes a fixed-descriptor composite device providing the USB CDC BASIC Console and USB Mass Storage at the same time. SD-card detection, read, create, overwrite, delete, and coexistence with the CDC Console have been verified on real Windows hardware.

The SD card is owned exclusively by either CPB firmware or the USB host. Normally, safely eject it on the host and select `Return SD to CPB`. `Force Disconnect` withdraws only the MSC media while preserving CDC, but is an emergency operation that can lose data still held in the host write cache.

### 9. YMODEM / XMODEM

YMODEM is recommended for normal serial file transfer. Version 0.8 supports single-file transfer, filename transfer, exact file size, 1 KiB data blocks, and transfers in both directions. Round trips have been verified with Tera Term. Multi-file batch transfer is not implemented.

XMODEM-CRC has also been verified in both directions with Tera Term. Because XMODEM does not carry a filename or exact original size, trailing `0x1A` padding may remain. Use YMODEM or the Wi-Fi File Server for normal transfers.

### 10. Wi-Fi File Server

Using Pico 2 W Wi-Fi, a browser on the same LAN can Upload, Download, Delete, and Refresh files on the SD card. Wi-Fi and the File Server are OFF at startup and must be started explicitly.

The Wi-Fi password is currently stored in plaintext in `RMBASIC.CFG` on the SD card. The distributed sample leaves SSID and password empty.

### 11. BASIC Language

Version 0.8 provides line-numbered programs, direct mode, numeric and string variables, numeric and string arrays, conditionals, loops, subroutines, `ON GOTO`, and `ON GOSUB`.

### 12. Graphics

Graphics include `SCREEN`, `CLS`, `COLOR`, `COLORHSV`, `PSET`, `LINE`, `CIRCLE`, `BOX`, `PAINT`, `LOCATE`, `GLOCATE`, `GPRINT`, `SAVEIMAGE`, `FLUSH`, and `SLEEP`. The full 320×320 LCD is available.

`Alt+S` saves a 320×320 24-bit BMP using automatic names such as `SCREEN0001.BMP`. The REPL `SCREENSHOT` command and BASIC `SAVEIMAGE` are also available.

### 13. Control Center / UI

- Three fixed status rows
- F1–F10 Quick Load / Run
- HOME (Shift+Tab) Control Center
- Battery, charging, date/time, Wi-Fi, Caps Lock, CPU profile, console routing, and RUN time

CPU profiles are FULL 150 MHz, NORMAL 100 MHz, and ECO 75 MHz. Startup uses FULL 150 MHz.

### 14. Build from Source

Use Pico SDK 2.3.1:

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -G Ninja -DPICO_BOARD=pico2_w
cmake --build build
```

The internal CMake target and output name remain `retrominibasic_picocalc` for compatibility.

### 15. Documentation

- [Version 0.8 Japanese manual](docs/manual-ja.md)
- [Version 0.8 Japanese PDF](docs/Cala-Pokecom-BASIC-v0.8-ja.pdf)
- [Roadmap](ROADMAP.md)

### 16. Project Lineage

Cala's Pokecom BASIC is based technically on RetroMiniBASIC, created by Cala Maclir, and was designed and implemented for ClockworkPi PicoCalc by the same author, Cala Maclir. It is not merely a third-party port.

Names such as `RetroMiniBASIC`, `RMB`, `RMBASIC`, and `retrominibasic_picocalc` remain internally for compatibility and historical reasons. The official user-facing name is **Cala's Pokecom BASIC**.

### 17. License

BSD 3-Clause License. See [LICENSE](LICENSE).
