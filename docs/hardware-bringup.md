# PicoCalc + Pico 2 W 導入・実機確認手順

対象：Cala's Pokecom BASIC Version 0.85、ClockworkPi PicoCalc、Raspberry Pi Pico 2 W。

## Portとmediaの名称

- Pico 2 W側Micro-USB：native USB、BOOTSEL UF2、CDC、MSC、reset vendor interface
- PicoCalc本体USB Type-C：電源／充電、mainboard CH340C経由UART0
- PicoCalc内蔵slot：**SD card**

## 初回UF2導入

1. artifact `CPokecombasic-pico2w`から`build/CPokecombasic.uf2`を取得します。
2. Pico 2 WをPicoCalcから外します。
3. Pico 2 WのBOOTSELを押しながらMicro-USBでPCへ接続します。
4. RPI-RP2 driveへUF2をcopyします。
5. Pico 2 WをPicoCalcへ戻し、FAT32のSD cardを装着して起動します。

Version 0.85導入後はFirmware menuまたは`flash-cpb.cmd`を使用できます。

## 基本確認

- bannerがCala's Pokecom BASIC Version 0.85を表示
- LCD 320×320、keyboard、Caps Lock、battery／charge、backlight
- status 3行とF1～F5 footer。Shift中はF6～F10
- Alt+Sが`SCREEN0001.BMP`形式でSD cardへ保存
- CPU 150／100／75 MHz
- Key Click OFF／SOFT／CLASSIC／SHARP

## SD and Program Storage

- AUTO、SD CARD、INTERNAL RAMの切替
- RAM：256×191、SD：1024×2047
- exact 2047-character source lineのLOAD／LIST／RUN／SAVE／reload
- 2048-character bodyの明示拒否、silent truncationなし
- long-line SD sourceのRAM切替が`LINE TOO LONG FOR RAM`、source保持
- dirty sessionだけが再起動後にrestored notice
- clean named／fresh UNTITLEDではnoticeなし

## USB

Pico 2 W側Micro-USBでCDC、MSC、reset vendor interfaceを確認します。USB Storage開始後、host側でread／create／overwrite／deleteを確認し、安全にEjectして`Return SD to CPB`を選びます。返却後にSD ProgramStoreがindexを再構築してLOAD／LISTできることを確認します。

Firmware操作前はUSB Storage ownershipをCPBへ戻します。

## Bluetooth

1. Control Center → Bluetooth → Enable Bluetooth
2. PCで`CPB-PicoCalc`へClassic SPP接続
3. Test Terminalでechoとtest message
4. Console ONでBASIC console
5. Console OFFでもFile TransferのBluetooth SPPをexplicit選択して転送

YMODEM receiveでは1 byte、4040 byte、2-file batch、5-file batchを確認し、Overflow RXが増えず`TRANSFER COMPLETE`になることを確認します。

## Audio and graphics

- `BEEP`
- 1～3 voice MML、`PLAY PAUSE/RESUME/WAIT/STOP`
- PCM WAV各対応format
- graphics／PAINT中のbackground audio
- WAV終了時に余分な音がないこと
- `GDEF` mono／indexed-color、2×2 composite PCG animation

## Firmware

- Control Center → Firmware → Enter BOOTSEL
- Control Center → Firmware → Reboot
- Windows `flash-cpb.cmd`
- 更新後もCDC／MSC／Bluetooth／SDを再確認

## 回帰確認

- Wi-Fi手動ON、SSID接続、NTP、File Server
- XMODEM send／receive
- YMODEM single send、single／batch receive
- LIST footerのscroll安定
- PAUSE／INKEY／BREAK／SLEEP
- STANDBY復帰
