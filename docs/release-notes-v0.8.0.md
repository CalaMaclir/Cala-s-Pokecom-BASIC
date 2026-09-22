# Cala's Pokecom BASIC Version 0.8

## 日本語

**Version 0.8は、Cala's Pokecom BASICとして初の一般公開リリースです。**

Cala's Pokecom BASICは、Cala Maclirが制作したRetroMiniBASICを技術的基盤として、同じCala Maclir本人がClockworkPi PicoCalc向けに設計・実装した、スタンドアロンのBASIC環境です。

### Highlights

1. **First Public Release**  
   行番号付きBASIC、直接実行、プログラム保存、グラフィック、通信、PicoCalc向けUIを統合した最初の公開版です。

2. **USB CDC + Mass Storage**  
   Pico 2 W基板上のMicro-USBから、固定descriptorのUSB CDC BASIC ConsoleとUSB Mass Storageを同時に提供します。WindowsでSDカード認識、read、create、overwrite、delete、およびCDCとの共存を実機確認済みです。

3. **Safe SD ownership**  
   SDカードはCPBまたはUSBホストの一方だけが所有します。通常はhost Safe Eject後に`Return SD to CPB`を使用してください。`Force Disconnect`はCDCを維持したままMSC mediaを切断しますが、未送信write cacheによるデータ損失の可能性がある緊急操作です。

4. **Program Storage**  
   `AUTO`、`SD CARD`、`INTERNAL RAM`を選択できます。SD-backedは最大1,024行、Internal RAMは最大256行です。USB返却後はbacking BASを再scan・verifyします。

5. **YMODEM / XMODEM**  
   推奨方式のYMODEMは単一ファイル、ファイル名、正確なサイズ、1 KiB block、PC ↔ PicoCalcに対応します。XMODEM-CRCも利用できます。両方式ともTera Termで実機往復確認済みです。YMODEMの複数ファイルbatchは未実装です。

6. **Wi-Fi File Server**  
   同一LANのブラウザからSDカードをUpload、Download、Delete、Refreshできます。Wi-FiとFile Serverは起動時OFFです。Wi-Fi passwordは`RMBASIC.CFG`へ平文保存されます。

7. **PicoCalc UI**  
   3行固定ステータス、F1～F10 Quick Load / Run、HOME（Shift+Tab）のControl Center、battery、charging、date/time、Wi-Fi、Caps Lock、CPU profile、console routing、RUN timeを提供します。

8. **BASIC / Graphics**  
   数値・文字列変数、数値・文字列配列、`IF / THEN / ELSE`、各種loop、`GOTO`、`GOSUB / RETURN`、`ON GOTO`、`ON GOSUB`を提供します。320×320 LCDで`SCREEN`、`CLS`、`COLOR`、`COLORHSV`、`PSET`、`LINE`、`CIRCLE`、`BOX`、`PAINT`、`LOCATE`、`GLOCATE`、`GPRINT`、`SAVEIMAGE`、`FLUSH`、`SLEEP`を利用できます。`Alt+S`は320×320・24-bit BMPを自動採番で保存します。

9. **Supported Hardware**  
   正式対応はClockworkPi PicoCalc + Raspberry Pi Pico 2 W（RP2350）です。Pico 2、Pico、Pico WはVersion 0.8の正式対応機種ではありません。USB MSCはPico 2 W Micro-USBから提供され、PicoCalc本体USB Type-Cからは提供されません。

10. **Documentation / License**  
    Version 0.8日本語マニュアルとPDFを同梱し、BSD 3-Clause Licenseで公開します。

### Release Assets

- `Cala-Pokecom-BASIC-v0.8-pico2w.uf2`
- `Cala-Pokecom-BASIC-v0.8-ja.pdf`
- `Cala-Pokecom-BASIC-v0.8-examples.zip`
- `SHA256SUMS.txt`

## English

**Version 0.8 is the first public release of Cala's Pokecom BASIC.**

Cala's Pokecom BASIC is a standalone BASIC environment based technically on RetroMiniBASIC, created by Cala Maclir, and designed and implemented for ClockworkPi PicoCalc by the same author, Cala Maclir.

### Highlights

1. **First Public Release**  
   The first public version integrating line-numbered BASIC, direct execution, program storage, graphics, communications, and a PicoCalc-oriented UI.

2. **USB CDC + Mass Storage**  
   The Micro-USB connector on the Pico 2 W board provides a fixed-descriptor USB CDC BASIC Console and USB Mass Storage simultaneously. SD-card detection, read, create, overwrite, delete, and coexistence with CDC have been verified on real Windows hardware.

3. **Safe SD ownership**  
   The SD card is owned by exactly one side: CPB or the USB host. Normally use host Safe Eject followed by `Return SD to CPB`. `Force Disconnect` withdraws MSC media while preserving CDC, but is an emergency operation that may lose data still held in the host write cache.

4. **Program Storage**  
   Select `AUTO`, `SD CARD`, or `INTERNAL RAM`. The SD-backed store supports up to 1,024 lines and the internal-RAM store up to 256 lines. The backing BAS file is rescanned and verified after return from USB.

5. **YMODEM / XMODEM**  
   Recommended YMODEM supports one file, filename transfer, exact size, 1 KiB data blocks, and both PC → PicoCalc and PicoCalc → PC. XMODEM-CRC is also available. Both directions have been verified with Tera Term. Multi-file YMODEM batch transfer is not implemented.

6. **Wi-Fi File Server**  
   A browser on the same LAN can Upload, Download, Delete, and Refresh SD-card files. Wi-Fi and the File Server are OFF at startup. The Wi-Fi password is stored in plaintext in `RMBASIC.CFG`.

7. **PicoCalc UI**  
   Three fixed status rows, F1–F10 Quick Load / Run, HOME (Shift+Tab) Control Center, battery, charging, date/time, Wi-Fi, Caps Lock, CPU profile, console routing, and RUN time.

8. **BASIC / Graphics**  
   Numeric and string variables and arrays, `IF / THEN / ELSE`, loops, `GOTO`, `GOSUB / RETURN`, `ON GOTO`, and `ON GOSUB`. Graphics use the full 320×320 LCD with `SCREEN`, `CLS`, `COLOR`, `COLORHSV`, `PSET`, `LINE`, `CIRCLE`, `BOX`, `PAINT`, `LOCATE`, `GLOCATE`, `GPRINT`, `SAVEIMAGE`, `FLUSH`, and `SLEEP`. `Alt+S` saves automatically numbered 320×320 24-bit BMP screenshots.

9. **Supported Hardware**  
   Official support is limited to ClockworkPi PicoCalc with Raspberry Pi Pico 2 W (RP2350). Pico 2, Pico, and Pico W are not officially supported by Version 0.8. USB MSC is provided through the Pico 2 W Micro-USB connector, not through the PicoCalc USB Type-C connector.

10. **Documentation / License**  
    Includes the Version 0.8 Japanese manual and PDF. Released under the BSD 3-Clause License.

### Release Assets

- `Cala-Pokecom-BASIC-v0.8-pico2w.uf2`
- `Cala-Pokecom-BASIC-v0.8-ja.pdf`
- `Cala-Pokecom-BASIC-v0.8-examples.zip`
- `SHA256SUMS.txt`
