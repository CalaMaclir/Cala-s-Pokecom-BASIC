# Cala's Pokecom BASIC Version 0.91

**for ClockworkPi PicoCalc + Raspberry Pi Pico 2 W**

[日本語](#日本語) | [English](#english) | [GitHub Releases](https://github.com/CalaMaclir/Cala-s-Pokecom-BASIC/releases)

## 日本語

### 概要

**Cala's Pokecom BASIC（CPB）**は、PicoCalcだけでプログラムの作成・実行・保存、グラフィック、音楽、通信を行えるスタンドアロンの行番号付きBASIC環境です。Version 0.91の正式対応構成は **ClockworkPi PicoCalc + Raspberry Pi Pico 2 W（RP2350）**です。

### Version 0.91の主な機能

- 独立Full-Screen BASIC Editor（visual wrap、Find、Goto、PSRAM Undo／Redo）
- `BASIC>`のsession-only command historyと、FilesのPROGRAMS／DIRECTORY管理
- PicoCalc PSRAMの自動検出、128 KiB editor history、diagnostics
- PCM WAV／MP3のbackground再生、3 voice MML、BEEP
- Bluetooth Classic／BLE HID Keyboard（JIS／US layout、再接続・登録解除）
- SD: 1,024行 × 2,047文字、RAM: 256行 × 191文字のProgram Storage
- Storage Continuity、transactional save、USB Storage return recovery
- XMODEM／YMODEM、USB CDC、UART0、USB Mass Storage、Wi-Fi HTTP File Server
- External RTC／I2C、Wi-Fi／NTP、Firmware menu、Windows用`flash-cpb.cmd`

### Download / Installation

正式配布は[Version 0.91 Release](https://github.com/CalaMaclir/Cala-s-Pokecom-BASIC/releases/tag/v0.91.0)です。通常利用では次のassetを使用します。

- `CPokecombasic-v0.91-build528-pico2w.zip`（完全build package。`flash-cpb.cmd`を同梱）
- `Cala-Pokecom-BASIC-v0.91-pico2w.uf2`
- `Cala-Pokecom-BASIC-v0.91-Install-Manual-ja.pdf`
- `Cala-Pokecom-BASIC-v0.91-System-Manual-ja.pdf`
- `Cala-Pokecom-BASIC-v0.91-Programming-Reference-ja.pdf`
- `Cala-Pokecom-BASIC-v0.91-examples.zip`
- `SHA256SUMS.txt`

初回はPico 2 WをPicoCalcから取り外し、BOOTSELを押しながらPico 2 W側Micro-USBでPCへ接続し、RPI-RP2へUF2をコピーします。対応版の導入後は`Control Center → Firmware → Enter BOOTSEL`も利用できます。Windows用`flash-cpb.cmd`は完全build package ZIP内にあり、展開後の`build/CPokecombasic.uf2`との相対配置を維持して使用します。

### Documentation

- [導入マニュアル](docs/install-manual-ja.md)（[PDF](docs/Cala-Pokecom-BASIC-v0.91-Install-Manual-ja.pdf)）
- [システムマニュアル](docs/system-manual-ja.md)（[PDF](docs/Cala-Pokecom-BASIC-v0.91-System-Manual-ja.pdf)）
- [プログラミング・リファレンス](docs/programming-reference-ja.md)（[PDF](docs/Cala-Pokecom-BASIC-v0.91-Programming-Reference-ja.pdf)）
- [Version 0.91 Release Notes](docs/release/v0.91-release-notes.md)
- [Roadmap](ROADMAP.md)

### Build from Source

Pico SDK 2.3.1を使用します。

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -G Ninja -DPICO_BOARD=pico2_w
cmake --build build
```

外向け成果物は`build/CPokecombasic.uf2`と`build/CPokecombasic.elf`です。

---

## English

### Overview

**Cala's Pokecom BASIC (CPB)** is a standalone, line-numbered BASIC environment for creating, running, and saving programs, graphics, music, and communications directly on a PicoCalc. Version 0.91 officially supports **ClockworkPi PicoCalc with Raspberry Pi Pico 2 W (RP2350)**.

### Version 0.91 highlights

- Full-screen BASIC editor with visual wrapping, Find/Goto, and PSRAM-backed Undo/Redo
- Session command history and expanded PROGRAMS/DIRECTORY file management
- Optional PicoCalc PSRAM detection, a 128 KiB editor history, and diagnostics
- Background PCM WAV/MP3 playback, three-voice MML, and BEEP
- Bluetooth Classic and BLE HID keyboards with JIS/US layouts
- Program Storage: 1,024 lines × 2,047 characters on SD; 256 × 191 in RAM
- Storage Continuity, transactional saves, XMODEM/YMODEM, USB storage, and Wi-Fi file server
- External RTC/I2C, Wi-Fi/NTP, firmware controls, and Windows `flash-cpb.cmd`

### Download / Installation

The official distribution is the [Version 0.91 Release](https://github.com/CalaMaclir/Cala-s-Pokecom-BASIC/releases/tag/v0.91.0). Use `Cala-Pokecom-BASIC-v0.91-pico2w.uf2` for normal installation. The complete `CPokecombasic-v0.91-build528-pico2w.zip` package contains the verified CI build, examples, diagnostics, and the Windows `flash-cpb.cmd` helper. The helper is not distributed as a standalone asset.

For the initial installation, remove the Pico 2 W from the PicoCalc, hold BOOTSEL while connecting its Micro-USB port to a PC, and copy the UF2 to RPI-RP2. Once a compatible version is installed, `Control Center → Firmware → Enter BOOTSEL` is also available. To use `flash-cpb.cmd`, extract the complete build package and preserve its relative path to `build/CPokecombasic.uf2`.

### Documentation

- [Japanese manuals index](docs/manual-ja.md)
- [Version 0.91 Release Notes](docs/release/v0.91-release-notes.md)
- [Roadmap](ROADMAP.md)

### Build from Source

Use Pico SDK 2.3.1:

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -G Ninja -DPICO_BOARD=pico2_w
cmake --build build
```

The external artifacts are `build/CPokecombasic.uf2` and `build/CPokecombasic.elf`.

### Lineage and License

CPB is based technically on RetroMiniBASIC, created by Cala Maclir, and was designed and implemented for PicoCalc by the same author. Internal compatibility names remain, while the product name is **Cala's Pokecom BASIC**. Licensed under the BSD 3-Clause License.

Copyright (C) 2026 Cala Maclir
