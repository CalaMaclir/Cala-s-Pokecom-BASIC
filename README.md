# Cala's Pokecom BASIC Version 0.91

**for ClockworkPi PicoCalc + Raspberry Pi Pico 2 W**

[日本語](#日本語) | [English](#english) | [GitHub Releases](https://github.com/CalaMaclir/Cala-s-Pokecom-BASIC/releases)

## 日本語

### 概要

**Cala's Pokecom BASIC（CPB）**は、PicoCalcだけでプログラムの作成・実行・保存、グラフィック、音楽、通信を行えるスタンドアロンの行番号付きBASIC環境です。Version 0.91の正式対応構成は **ClockworkPi PicoCalc + Raspberry Pi Pico 2 W（RP2350）**です。

### Version 0.91の主な機能

- 独立Full-Screen BASIC Editor（visual wrap、Find、Goto、PSRAM Undo／Redo）
- `BASIC>` command historyとPROGRAMS／DIRECTORY file management
- PSRAM multi-client architecture：Editor History、INTERNAL ProgramStore、SD Cache、DirectState、Compiled Cache
- PSRAM利用時のINTERNAL／SDはいずれも最大1,024行 × 2,047文字
- PSRAMなしのINTERNALは256行 × 191文字のSRAM fallback
- USB CDC／UART0 file transferのbulk化、UART最大921600 bps、任意RX DMA
- XMODEM／YMODEM、USB Mass Storage、Wi-Fi HTTP File Server
- Bluetooth Classic／BLE HID Keyboard（JIS／US layout）
- PCM WAV／MP3 background再生、3 voice MML、BEEP
- `LIST`をBREAK／Esc／Ctrl-Cで停止可能
- LOAD時の`LOADING... (filename)`、RUN開始時の`RUN...`
- External RTC／I2C、Wi-Fi／NTP、Firmware menu、Windows用`flash-cpb.cmd`

### Download / Installation

正式配布は[Version 0.91 Release](https://github.com/CalaMaclir/Cala-s-Pokecom-BASIC/releases/tag/v0.91.0)です。

- `CPokecombasic-v0.91-build664-pico2w.zip`（完全build package。Windows用`flash-cpb.cmd`を同梱）
- `Cala-Pokecom-BASIC-v0.91-pico2w.uf2`
- `Cala-Pokecom-BASIC-v0.91-Install-Manual-ja.pdf`
- `Cala-Pokecom-BASIC-v0.91-System-Manual-ja.pdf`
- `Cala-Pokecom-BASIC-v0.91-Programming-Reference-ja.pdf`
- `SHA256SUMS.txt`

`flash-cpb.cmd`は単独assetではありません。完全build packageを展開し、`build/CPokecombasic.uf2`との相対配置を維持して使用します。

### Documentation

- [導入マニュアル](docs/install-manual-ja.md)
- [システムマニュアル](docs/system-manual-ja.md)
- [プログラミング・リファレンス](docs/programming-reference-ja.md)
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
- Multi-client PSRAM architecture for editor history, INTERNAL ProgramStore, SD cache, DirectState, and compiled cache
- Up to 1,024 lines x 2,047 characters for SD and PSRAM-backed INTERNAL storage
- 256 x 191 SRAM fallback when PSRAM is unavailable
- Bulk USB CDC/UART file transfer, UART up to 921600 bps, optional RX DMA
- XMODEM/YMODEM, USB Mass Storage, and Wi-Fi file server
- Bluetooth Classic and BLE HID keyboards
- Background WAV/MP3 playback, three-voice MML, and BEEP
- Interruptible LIST and progress indication for LOAD/RUN

### Download / Installation

The official distribution is the [Version 0.91 Release](https://github.com/CalaMaclir/Cala-s-Pokecom-BASIC/releases/tag/v0.91.0). The complete `CPokecombasic-v0.91-build664-pico2w.zip` package contains the verified CI build, examples, diagnostics, and the Windows `flash-cpb.cmd` helper. The helper is not distributed as a standalone asset.

### Documentation

- [Install Manual (Japanese)](docs/install-manual-ja.md)
- [System Manual (Japanese)](docs/system-manual-ja.md)
- [Programming Reference (Japanese)](docs/programming-reference-ja.md)
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
