# USB CDC + MSC Stage 2（Version 0.8開発・検証記録）

基準：USB MSC Stage 1 merge `cc62e7db930efe370beb2f5ca10be21d8da3bb73`。
Stage 2では、PicoCalcのSDカードをPico 2 W側USBのMSCへ512-byte raw block deviceとして接続します。
CDC + MSCの固定USB記述子、SDK管理のUSB初期化・IRQ処理、XMODEM／YMODEMはStage 1から維持します。

> WindowsからPico 2 WのMicro-USBへ接続し、SDカードの認識、read、create、overwrite、deleteを含む一連のMSC操作と、BASIC CDCとの共存を実機確認済みです。Version 0.8正式版ではSafe Return／Force Disconnectを含むStage 2を採用しています。host testとCIは所有権遷移の回帰を継続検査します。

## 接続するUSB

MSCを利用できるのは、Pico 2 W基板上のネイティブUSB（Micro-USB）です。
PicoCalc本体のUSB Type-CはCH340C経由のUART0であり、USB MSCではありません。

## 所有権モデル

SDカードには、排他ロックとは別に次の長寿命所有権があります。

| 状態 | FatFs／BASIC | USB MSC | 復帰方法 |
| --- | --- | --- | --- |
| `Firmware` | 使用可 | No Media | Control Centerから開始 |
| `UsbHost` | 使用不可 | raw block read/write可 | PC側でsafe eject |
| `Transition` | 使用不可 | 新規I/O不可 | foregroundで完了処理 |
| `Unavailable` | 使用不可 | No Media | 原因確認後、Control Centerで明示復旧 |

HTTP転送などの短時間の`storage busy`と、USBホストがカードを保持する`Owner`は別の状態です。
USB callbackはFatFs、mount、ProgramStoreを呼びません。IRQ側はmedia stateのatomic更新と512-byte raw I/Oだけを行い、unmount／remount／ProgramStore再構築はforegroundで行います。

## USB Storage開始

Control Centerの`USB Storage`から明示的に開始します。

1. HTTP File Serverを停止します。
2. storage busy、SDカード、ProgramStore状態を確認します。
3. SD-backed ProgramStoreがdirtyなら`Save`／`Discard`／`Cancel`を要求します。
4. SD-backed ProgramStoreをsuspendし、active-file guardを解除します。
5. FatFsをunmountし、block deviceをsyncします。
6. 容量が512-byte単位かつ32-bit LBA範囲であることを確認します。
7. 所有権を`UsbHost`へ移し、MSC mediaをpresentにします。

途中で失敗した場合は、可能なら従来のfirmware mountへロールバックします。新しい所有権が成立する前にUSB mediaをpresentにしません。

## USBホスト所有中

- ステータスは`SD:USB`です。
- `DIR`、`LOAD`、`SAVE`、SD-backed source access、スクリーンショット、XMODEM／YMODEM、HTTP File Server、設定保存などのFatFs操作は拒否されます。
- RAM ProgramStoreのLIST／RUN／編集は継続できます。
- Wi-Fi接続自体は維持できますが、File Serverは停止したままです。
- STANDBYは拒否されます。
- 通常の返却はControl Centerの`Return SD to CPB`を使います。PC側でドライブをsafe ejectするまでは返却を拒否します。
- 緊急時だけ`Force Disconnect`を使用できます。hostの未送信write cacheを失う可能性があります。

READ10／WRITE10はLUN 0、offset 0、ちょうど512 bytes、capacity内のLBAだけを受理します。LBAからbyte addressへの変換は64-bitで行います。ファイル全体や追加のSD cacheはRAMへ保持しません。

## Safe Return、Force Disconnect、異常終了

通常はPC側でUSB diskをsafe ejectしてから、Control Centerの`Return SD to CPB`を選びます。safe eject前のReturnは`PLEASE EJECT USB DISK ON HOST FIRST`として拒否し、SD所有権をUSB hostから移しません。safe ejectはMSC mediaをNo Mediaにしますが、CDC BASIC Consoleは維持します。

`Force Disconnect`はhostをejectできない場合の緊急操作です。確認画面の初期選択は`Cancel`で、実行時はMSC mediaだけをNo Mediaにします。USBデバイス全体をdisconnect／re-enumerateせず、CDCを維持します。新しいREAD10／WRITE10を拒否し、実行中のraw I/O完了を待ってからblock deviceをsyncし、FatFsをremountします。hostに未送信write cacheが残っている場合は、ファイルシステムやデータを失う可能性があります。

Safe ReturnとForce Disconnectのどちらでも、SD-backed ProgramStoreはUSB開始前のoffset／hash indexを再利用しません。backing BASを再scanして新しいindexをtransactionalに作り、host側でbacking fileが削除・破損していた場合はsuspended状態を維持します。

USBケーブル切断、SDカード取り外し、raw I/O errorは`Unavailable`へ移行します。データ保護のため自動remountせず、Control Centerの`Return SD to CPB`で明示的に復旧します。

## host test

CIでは次を検査します。

- 固定CDC + MSC descriptor、endpoint、No Media、100回の接続状態遷移
- 512-byte READ10／WRITE10、capacity、LBA／offset／size境界、read/write error sense
- unmount失敗のロールバック、safe eject前のReturn拒否、safe eject後のReturn、Force Disconnect、in-flight raw I/O待機、新規I/O拒否、remount失敗、unsafe disconnect、SD removal、raw I/O error、手動復旧
- ACTIVEメニューの`Return SD to CPB`／`Force Disconnect`／単一の`Back`、Force確認の初期`Cancel`、CDC非切断
- SD ProgramStoreのdirty拒否、USB後のfresh index rebuild、host変更・削除後のsuspend
- 既存のBASIC、ProgramStore、HTTP、XMODEM／YMODEMテスト
- Stage 1とStage 2の`.data`、`.bss`、`sizeof(Repl)`およびstack-usage比較

## 実機確認手順

1. 最新成功CIのUF2をPico 2 Wへ書き込み、Pico 2 W側USBをPCへ接続します。
2. CDC COMポートで`BASIC>`が動作することを確認します。
3. Control Center → `USB Storage` → `Enable USB Storage`を選択します。
4. PCでSDカード容量・既存ファイルを確認し、小さいBASとBMPを双方向コピーします。
5. コピー中、CPB側のSDコマンド、File Server、XMODEM／YMODEM、スクリーンショットが拒否されることを確認します。RAM ProgramStoreのLIST／RUN／編集は確認できます。
6. safe eject前に`Return SD to CPB`を選び、返却を拒否することを確認します。
7. PC側でドライブをsafe ejectし、`Return SD to CPB`で`SD:OK`へ戻ること、DIR／LOAD／SAVEが再び使えることを確認します。
8. PC側でbacking BASを変更・削除した場合のProgramStore再scan／suspendを確認します。
9. バックアップ済みのテスト用SDカードで確認画面の初期値が`Cancel`であることを確認し、`Force Disconnect`後もCDCコンソールが継続し、SDがCPBへ戻ることを確認します。
10. 別のテスト用SDカードで、ケーブル切断、カード取り外し、書込み中断を試し、自動remountせず明示復旧を要求することを確認します。
11. FULL／NORMAL／ECO、Wi-Fi boot OFF、File Server、XMODEM／YMODEM、STANDBY、3行status、Fキーを回帰確認します。

書込み中断試験は破損してもよいバックアップ済みSDカードだけで行います。
