# Cala's Pokecom BASIC Version 0.94

For ClockworkPi PicoCalc with Raspberry Pi Pico 2 W. Copyright (C) 2026 Cala Maclir.

v0.94 is the formal release with image I/O, Files audio ownership, diagnostics, Structured auto-indent, unsaved-program protection, and daily string/RTC functions. The project owner accepted the final Stage 1-11 hardware candidate on 2026-10-04. See the release checklist for the accepted candidate and publication-build mapping.

[日本語](README.md) | [Public releases](https://github.com/CalaMaclir/Cala-s-Pokecom-BASIC/releases)

## Install and run

Extract the complete CPokecombasic-v0.94-build<number>-pico2w.zip. Flash build/CPokecombasic.uf2 using BOOTSEL. On Windows, keep flash-cpb.cmd at ZIP root and UF2 in build/, put picotool.exe on PATH, close serial terminals and return USB Storage to CPB before updating. flash-cpb.cmd is included only in the complete ZIP, not as a standalone release asset.

At BASIC>, enter:

```basic
10 PRINT "HELLO CPB"
20 END
RUN
SAVE "HELLO"
```

For Structured BASIC, choose Control Center → Editor → New Program → Structured BASIC. Edit this source, save with F1, exit with Esc and RUN; the result is 144.

```basic
PRINT SQUARE(12)
FUNCTION SQUARE(X)
    RETURN X*X
END FUNCTION
```

## Version 0.94 highlights

LOADIMAGE / SAVEIMAGE and the SAVE IMAGE compatibility alias work in both stored BASIC modes. INFO / LASTERROR are REPL commands. DATE$() / TIME$(), TRIM$ / LTRIM$ / RTRIM$ / REPLACE$ / SPACE$ are available. NEW / LOAD / Files / quick keys share Save / Discard / Cancel protection.

v0.91 development improvements are included: bulk USB CDC/UART transfer, 921600-bps UART and optional RX DMA, PSRAM ProgramStore/DirectState/Compiled Cache, LIST interruption and menu organization. Reported v0.91 device throughput was approximately 33 KB/s PC-to-PicoCalc and 62 KB/s in reverse with YMODEM DMA at 921600 bps; this is not a new v0.92 measurement.

v0.92 added LCD standby/wake, editor split/insert/join with manual Classic numbering, directory management, Structured BASIC, block IF/ELSEIF/ELSE/END IF and typed FUNCTION. Version 0.93 added prompt shortcuts Alt+E/R/C, Editor Outdent/Alt+M/Alt+R, SELECT CASE, EXIT FOR/DO, DATA/READ/RESTORE, LOG/LN/ASIN/ACOS/ATAN2, integer division and bit operators. LOG is now base 10; use LN when migrating programs that used the former natural-log LOG behavior. Numbers remain single-precision floats; there is no INTEGER type or A%.

Hardware work adds keyboard-I2C diagnostics/passive recovery, storage/standby ownership checks, and a combined regression sample. Wi-Fi supports up to five Enabled/Disabled profiles, deletion, a five-second scan observation, strongest-known-AP selection with fallback, and ESC/BREAK cancellation. Password entry is visible while typing on the small screen; stored passwords are not displayed by menus or diagnostics. Master Wi-Fi always starts OFF.

## Storage and limits

| Backend | Source rows | Characters per row |
|---|---:|---:|
| INTERNAL PSRAM / SD | 1024 | 2047 |
| INTERNAL SRAM fallback | 256 | 191 |

Prompt/Direct input remains 191 characters. Editor Undo/Redo needs PSRAM and a 256 KiB history allocation. Structured numbers (00001  code) are display-only; continuation number fields are blank and saved BAS/LIST are unnumbered. Tab inserts spaces to four-column stops.

FUNCTION: 32 functions, 8 parameters, 128 scalar locals including parameters, call depth16, strings127 characters, globals64. IL1536 slots and string pool6144 bytes can limit programs before source capacity. Large string frames may exhaust SRAM before depth16. Arrays inside functions, SUB, labels, STATIC, OTA and native BASIC compilation are unsupported.

Back up BAS/config before updating. Session version1 loads as Classic and migrates to version2; Compiled Cache format6 is separate and old/corrupt entries become misses. Routine SD erasure is unnecessary. Standby preserves RAM and is not power-off. LCD dark/wake and ten cycles were verified on Keyboard BIOS1.7; updating BIOS is not a universal installation requirement.

## Manuals and samples

[Japanese manual index](docs/manual-ja.md). PDFs are stored in docs/ and included in the complete Actions build artifact ZIP.

- [Install (Japanese)](docs/install-manual-ja.md) / [v0.94 PDF](docs/Cala-Pokecom-BASIC-v0.94-Install-Manual-ja.pdf)
- [System (Japanese)](docs/system-manual-ja.md) / [v0.94 PDF](docs/Cala-Pokecom-BASIC-v0.94-System-Manual-ja.pdf)
- [Programming Reference (Japanese)](docs/programming-reference-ja.md) / [v0.94 PDF](docs/Cala-Pokecom-BASIC-v0.94-Programming-Reference-ja.pdf)
- [Release notes](docs/release/v0.94-release-notes.md)
- [Known limitations](docs/release/v0.94-known-limitations.md)
- [Examples and expected results](examples/README.md)

Release assets: complete Actions ZIP, identical standalone normal UF2, three Japanese PDFs and SHA256SUMS.txt. The updater is provided only within the complete ZIP.

## Build and license

Use Pico SDK2.3.1 and pinned CMake dependencies.

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -G Ninja -DPICO_BOARD=pico2_w
cmake --build build
```

[BSD 3-Clause](LICENSE). Preserve third_party/minimp3/LICENSE (CC0) and upstream dependencies' notices. See [third-party notices](docs/THIRD_PARTY_NOTICES.md).


## Development rules

See [public source build and validation](docs/development/public-source-build.md) before modifying the source.
