# Cala's Pokecom BASIC Roadmap

現在の正式版は**Version 0.85**です。対象hardwareはClockworkPi PicoCalc + Raspberry Pi Pico 2 Wです。

## Completed

### Version 0.8

- 初公開
- USB CDC + Mass Storage composite device
- AUTO／SD CARD／INTERNAL RAM Program Storage
- XMODEM／YMODEM single-file transfer
- Wi-Fi HTTP File Server

### Version 0.81

- `PAUSE`、`INKEY`、runtime input
- BREAK responsivenessとinterruptible `SLEEP`
- current-file `SAVE`とControl Center保存操作

### Version 0.82

- External PCF8563 RTCとI2C
- `I2C SCAN`、`I2CREAD`、`I2CWRITE`
- `&Hxxxx` hexadecimal literal
- PCG：`GDEF`、`GPRINT`、`GLOCATE`、`GPALETTE`

### Version 0.83

- `BEEP`
- 最大3 voiceのMML `PLAY`
- PCM WAV playback
- background audio

### Version 0.84

- SD editing session continuity
- dirty session recovery
- transactional save／backup／power-loss recovery
- USB Storage返却後のProgramStore index rebuild

### Version 0.85

- Bluetooth Classic SPP／RFCOMM
- Bluetooth Console、Test Terminal、exclusive RX ownership
- Bluetooth XMODEM／YMODEM transport
- File Transfer route selector：AUTO／USB CDC／UART0／Bluetooth SPP
- YMODEM multi-file batch receive、exact-size commit、timeout／duplicate Block 0 recovery
- RX 8192 bytes、TX 2048 bytes、transfer prefetch 2048 bytes
- Firmware → Enter BOOTSEL／Reboot
- Windows `flash-cpb.cmd`とRaspberry Pi reset vendor interface
- graphics実行中のbackground audio service、6-buffer audio queue、PAINT cooperation
- LIST footer scroll改善、WAV tail ramp
- Key Click：OFF／SOFT／CLASSIC／SHARP
- SD Program Storage：1024 lines × 2047 body characters
- 外向けartifactを`CPokecombasic`へ統一

## Future candidates

- 実機runtime heap／stack high-water measurement
- YMODEM sender側のmulti-file batch
- Bluetooth RFCOMM timingの継続的な実機互換性評価
- 既存機能を損なわない範囲でのcompiler／VM／graphics／network buffer最適化

将来候補は実装順序やrelease時期を保証しません。

## Deferred

- Direct mode配列のcommand間永続化
- 現行`STANDBY`より深いhardware suspend／resume

## Intentionally not planned

`LOADURL`／`RUNURL`は実装しません。安全なtransferと実行を分離するため、USB Storage、Wi-Fi File Server、YMODEM、XMODEMを使用します。

## Design priorities

1. データ保全
2. スタンドアロン動作の安定性
3. PicoCalc標準hardwareとの整合
4. 予測可能なmemory使用量
5. storage／通信障害からの復旧
6. 明確なuser operation
7. performance

