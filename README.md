# Cala's Pokecom BASIC Version 0.85

**for ClockworkPi PicoCalc + Raspberry Pi Pico 2 W**

[日本語](#日本語) | [English](#english) | [GitHub Releases](https://github.com/CalaMaclir/Cala-s-Pokecom-BASIC/releases)

## 日本語

### 概要

**Cala's Pokecom BASIC（CPB）**は、PicoCalcだけでプログラムの作成・実行・保存、グラフィック、音楽、通信を行えるスタンドアロンの行番号付きBASIC環境です。Version 0.85の正式対応構成は **ClockworkPi PicoCalc + Raspberry Pi Pico 2 W（RP2350）**です。

### Version 0.85の主な機能

- 数値・文字列変数、数値・文字列配列、`GOSUB`、`ON GOTO`、`ON GOSUB`
- 320×320 graphics、`PAINT`、8×8 mono／indexed-color PCG
- 3 voice MML、BEEP、PCM WAVのbackground再生
- AUTO／SD CARD／INTERNAL RAM Program Storage
- SD: 1,024行 × 2,047文字、RAM: 256行 × 191文字
- Storage Continuity、transactional save、USB Storage return recovery
- Bluetooth Classic SPP console、XMODEM／YMODEM file transfer
- YMODEM PC→PicoCalc single-file／multi-file batch受信、exact-size保存
- USB CDC、UART0、USB Mass Storage、Wi-Fi HTTP File Server
- Firmware menu、Windows用`flash-cpb.cmd`
- External RTC／I2C、Wi-Fi／NTP、F1～F10、Alt+S screenshot

### Download / Installation

正式配布は[GitHub Releases](https://github.com/CalaMaclir/Cala-s-Pokecom-BASIC/releases)です。Version 0.85では次のassetを使用します。

- `Cala-Pokecom-BASIC-v0.85-pico2w.uf2`
- `Cala-Pokecom-BASIC-v0.85-ja.pdf`
- `Cala-Pokecom-BASIC-v0.85-examples.zip`
- `flash-cpb.cmd`
- `SHA256SUMS.txt`

初回はPico 2 WをPicoCalcから取り外し、BOOTSELを押しながらPico 2 W側Micro-USBでPCへ接続し、RPI-RP2へUF2をコピーします。Version 0.85導入後は`Control Center → Firmware → Enter BOOTSEL`と`flash-cpb.cmd`も利用できます。古いfirmwareからの最初の移行だけは物理BOOTSEL操作が必要です。

| 端子 | 用途 |
|---|---|
| Pico 2 W Micro-USB | BOOTSEL、UF2、USB CDC、USB Mass Storage |
| PicoCalc USB Type-C | 電源、充電、CH340C経由UART0 |

### Program Storage

| Mode | 最大行数 | 1行本文 |
|---|---:|---:|
| INTERNAL RAM | 256 | 191 characters |
| SD CARD | 1,024 | 2,047 characters |
| AUTO | 選択backendに従う | 選択backendに従う |

本文長は行番号と直後のspaceを含みません。interactive line editorとDirect modeは191文字です。SDの長い行は`LOAD`、`LIST`、`RUN`、`SAVE`、session restoreに対応します。長い行を含むSD programをRAMへ切り替える操作は`LINE TOO LONG FOR RAM`で拒否し、元のSD sourceを保持します。

### Bluetooth / File Transfer

BluetoothはClassic SPP／RFCOMMです。device nameは`CPB-PicoCalc`で、BLEではありません。File Transfer transportはAUTO／USB CDC／UART0／Bluetooth SPPです。

- XMODEM: single-file send／receive
- YMODEM: single-file send、single-file／multi-file batch receive
- YMODEM receiveはheader sizeを使い、末尾paddingを保存しません

### Documentation

- [Version 0.85 日本語マニュアル](docs/manual-ja.md)
- [Version 0.85 日本語PDF](https://github.com/CalaMaclir/Cala-s-Pokecom-BASIC/releases/download/v0.85.0/Cala-Pokecom-BASIC-v0.85-ja.pdf)
- [Version 0.85 Release Notes](docs/release/v0.85-release-notes.md)
- [Version 0.85 Hardware Verification](docs/release/v0.85-hardware-verification.md)
- [Roadmap](ROADMAP.md)

### Build from Source

Pico SDK 2.3.1を使用します。

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -G Ninja -DPICO_BOARD=pico2_w
cmake --build build
```

外向け成果物は`build/CPokecombasic.uf2`と`build/CPokecombasic.elf`です。内部targetの`retrominibasic_picocalc`は互換性のため残しています。

### 名称とLicense

CPBはCala Maclirが制作したRetroMiniBASICを技術的基盤として、同じCala Maclir本人がPicoCalc向けに設計・実装したものです。内部名称や互換file名に`RetroMiniBASIC`、`RMB`、`RMBASIC`が残りますが、製品名は**Cala's Pokecom BASIC**です。LicenseはBSD 3-Clauseです。

---

## English

### Overview

**Cala's Pokecom BASIC (CPB)** is a standalone, line-numbered BASIC environment for creating, running, and saving programs, graphics, music, and communications directly on a PicoCalc. Version 0.85 officially supports **ClockworkPi PicoCalc with Raspberry Pi Pico 2 W (RP2350)**.

### Version 0.85 highlights

- Numeric and string variables and arrays, `GOSUB`, `ON GOTO`, and `ON GOSUB`
- 320×320 graphics, `PAINT`, and 8×8 monochrome/indexed-color PCG
- Three-voice MML, BEEP, and background PCM WAV playback
- AUTO, SD CARD, and INTERNAL RAM Program Storage
- SD: 1,024 lines × 2,047 characters; RAM: 256 lines × 191 characters
- Storage Continuity, transactional saves, and USB-storage return recovery
- Bluetooth Classic SPP console and XMODEM/YMODEM transfers
- YMODEM PC-to-PicoCalc single-file and multi-file batch receive with exact-size storage
- USB CDC, UART0, USB Mass Storage, and Wi-Fi HTTP File Server
- Firmware menu and Windows `flash-cpb.cmd`
- External RTC/I2C, Wi-Fi/NTP, F1–F10 shortcuts, and Alt+S screenshots

### Download / Installation

[GitHub Releases](https://github.com/CalaMaclir/Cala-s-Pokecom-BASIC/releases) is the official distribution channel. Version 0.85 provides the following assets:

- `Cala-Pokecom-BASIC-v0.85-pico2w.uf2`
- `Cala-Pokecom-BASIC-v0.85-ja.pdf`
- `Cala-Pokecom-BASIC-v0.85-examples.zip`
- `flash-cpb.cmd`
- `SHA256SUMS.txt`

For the initial installation, remove the Pico 2 W from the PicoCalc, hold BOOTSEL while connecting the Pico 2 W Micro-USB port to a PC, and copy the UF2 to RPI-RP2. After installing Version 0.85, you can also use `Control Center → Firmware → Enter BOOTSEL` and `flash-cpb.cmd`. The first migration from older firmware still requires physical BOOTSEL.

| Connector | Purpose |
|---|---|
| Pico 2 W Micro-USB | BOOTSEL, UF2 flashing, USB CDC, USB Mass Storage |
| PicoCalc USB Type-C | Power, charging, UART0 through CH340C |

### Program Storage

| Mode | Maximum lines | Line body |
|---|---:|---:|
| INTERNAL RAM | 256 | 191 characters |
| SD CARD | 1,024 | 2,047 characters |
| AUTO | Follows selected backend | Follows selected backend |

The limits exclude the line number and following space. Interactive editing and Direct mode remain limited to 191 characters. Long SD lines support `LOAD`, `LIST`, `RUN`, `SAVE`, and session restore. Switching a program containing long lines to RAM is rejected with `LINE TOO LONG FOR RAM`, preserving the SD source.

### Bluetooth / File Transfer

Bluetooth uses Classic SPP/RFCOMM, not BLE. The device name is `CPB-PicoCalc`. File Transfer transports are AUTO, USB CDC, UART0, and Bluetooth SPP.

- XMODEM: single-file send and receive
- YMODEM: single-file send; single-file and multi-file batch receive
- YMODEM receive uses the header size and does not retain trailing padding

### Documentation

- [Version 0.85 Japanese manual](docs/manual-ja.md)
- [Version 0.85 Japanese PDF](https://github.com/CalaMaclir/Cala-s-Pokecom-BASIC/releases/download/v0.85.0/Cala-Pokecom-BASIC-v0.85-ja.pdf)
- [Version 0.85 Release Notes](docs/release/v0.85-release-notes.md)
- [Version 0.85 Hardware Verification](docs/release/v0.85-hardware-verification.md)
- [Roadmap](ROADMAP.md)

### Build from Source

Use Pico SDK 2.3.1:

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -G Ninja -DPICO_BOARD=pico2_w
cmake --build build
```

The external artifacts are `build/CPokecombasic.uf2` and `build/CPokecombasic.elf`. The internal `retrominibasic_picocalc` target remains for compatibility.

### Lineage and License

CPB is based technically on RetroMiniBASIC, created by Cala Maclir, and was designed and implemented for PicoCalc by the same author. Internal and compatibility names such as `RetroMiniBASIC`, `RMB`, and `RMBASIC` remain, while the product name is **Cala's Pokecom BASIC**. Licensed under the BSD 3-Clause License.

Copyright (C) 2026 Cala Maclir

