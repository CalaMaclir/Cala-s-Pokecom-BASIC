# Cala's Pokecom BASIC Roadmap

この文書は、Cala's Pokecom BASICのリリース済み機能と今後の検討項目を整理します。

This document summarizes released features and future work for Cala's Pokecom BASIC.

> 現在の正式版：Version 0.8 / `main`
>
> Current stable release: Version 0.8 / `main`

PicoCalc実機での安定性、予測可能なメモリ使用量、データ保護を優先します。将来項目は実装順序やリリース時期を保証するものではありません。

Hardware stability, predictable memory use, and data integrity take priority. Future items do not guarantee an implementation order or release date.

---

## Version 0.8 - Released / 実装済み

### USB CDC + Mass Storage Composite Device

Pico 2 WのネイティブUSB（Micro-USB）を、固定descriptorのCDC + MSC Composite Deviceとして実装しました。

The Pico 2 W native USB port is implemented as a fixed CDC + MSC composite device.

- USB CDC BASIC Console
- PicoCalc SDカードを公開するUSB Mass Storage
- MSC ON／OFF時もUSB deviceを再enumerateしない固定descriptor
- WindowsでSDカード認識、read、create、overwrite、deleteを実機確認
- USB CDCとMSCの共存を実機確認

PicoCalc本体USB Type-CはCH340C経由UART0および電源・充電用です。Version 0.8のUSB MSCはPico 2 WのMicro-USBから提供します。

### Safe SD ownership / 安全なSD所有権管理

SDカードは常にCPBまたはUSBホストのどちらか一方だけが所有します。

The SD card is owned by exactly one side: CPB firmware or the USB host.

- FatFsをunmountしてからMSC mediaを公開
- MSC ACTIVE中はCPB側FatFsアクセスを拒否
- host Safe Eject後の`Return SD to CPB`
- 緊急用`Force Disconnect`
- Force確認画面の初期値は`Cancel`
- Force時もCDC interfaceは維持
- 新しいREAD10／WRITE10を停止し、実行中raw I/O完了後にsync／remount
- unsafe disconnect、card removal、raw I/O errorの明示復旧

`Force Disconnect`はhostの未送信write cacheを失う可能性があるため、通常操作には使用しません。

### ProgramStore coordination / ProgramStore連携

Version 0.75で導入したAUTO／SD CARD／INTERNAL RAMを維持し、USB Storageと統合しました。

- INTERNAL RAM backendはMSC中もSDを使わないLIST／RUN／編集が可能
- SD backendはMSC開始前にsuspend
- USB返却後は古いoffset／hash indexを再利用しない
- backing BASを再scan／verifyしてtransactionalに再開
- host側で削除・破損した場合はsuspended状態を維持

### Version 0.75までの主要機能 / Existing features

- RAM 256行／SD 1,024行のProgram Storage
- RUN／direct mode共有workspace
- 単一ファイルYMODEM exact-size transfer
- XMODEM-CRC compatibility transfer
- Control Center File Transfer
- Wi-Fi HTTP File Server
- 3行status、固定Fキー行、CPU FULL／NORMAL／ECO
- graphics、PAINT、3DHAT、Alt+S screenshot、STANDBY

---

## Future candidates / 将来候補

### YMODEM multi-file batch

Version 0.8のYMODEMは単一ファイル転送です。複数ファイルbatchは未実装で、将来候補として扱います。

Version 0.8 supports single-file YMODEM only. Multi-file batch transfer remains a future candidate.

### Memory architecture / メモリ構造

機能やデータ保護を損なわない範囲で、次の領域のライフタイムと共有可能性を継続調査します。

- numeric／string array pools
- graphics／PAINT workspace
- compiler／VM workspace
- network／lwIP buffers
- XMODEM／YMODEM／USB transfer buffers

### Runtime memory measurement

CIはELF／MAP、static SRAM、object size、compiler stack-usage estimateを記録します。実機runtime heap／stack high-water measurementは未実装です。

---

## Deferred / 見送り

### Persistent arrays in direct mode

直接モードのスカラー変数はコマンド間で保持されますが、数値配列・文字列配列はコマンドごとにリセットされます。配列の永続化はVersion 0.8には含まれません。

### Full hardware Suspend / Resume

現在の`STANDBY`はRAMを保持してRP2350のlow-power sleepを使用します。より深いhardware suspend／resumeはVersion 0.8には含まれません。

---

## Intentionally not planned / 実装しない機能

### LOADURL / RUNURL

`LOADURL`と`RUNURL`は実装しません。TLS証明書検証を伴わないURL取得や、取得したプログラムの暗黙実行は行いません。

ファイル転送には次を使用します。

- USB Mass Storage
- Wi-Fi File Server
- YMODEM
- XMODEM

Transfer、Load、Runは明示的に分離します。

---

## Design priorities / 設計優先順位

1. データ保全 / Data integrity
2. スタンドアロン動作の安定性 / Stable standalone operation
3. ClockworkPi PicoCalc標準ハードウェアとの整合 / Standard PicoCalc compatibility
4. 予測可能なメモリ使用量 / Predictable memory usage
5. SDカード・通信障害からの復旧 / Recovery from storage and communication failures
6. 明確なユーザー操作 / Clear user-visible behavior
7. 性能 / Performance

Cala's Pokecom BASICは、Wi-FiやPCがなくても単体で利用できるポケットコンピュータ型BASIC環境を維持します。
