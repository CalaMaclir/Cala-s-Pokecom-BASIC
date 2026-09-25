# USB CDC + MSC Stage 1（Version 0.8開発記録）

基準コミット：`b65c0b96e64d6e85a3641a509c2b2c93ebe12a9b`（`v0.8`）。
Stage 1ではMSCインターフェースを追加しますが、SDカードをPCへ公開しません。
CDC + MSC複合デバイスの列挙とCDCコンソールは実機確認済みです。
Stage 1はNo Media固定であり、SDカードの実データ転送は対象外です。
Version 0.8全体の完成を示すものではありません。

## USB構成

Pico 2 W側のUSBに接続します。PicoCalc側USB Type-CのUART経路ではMSCは利用できません。
USB 2.0 Full Speed、IAD付きのCDC + MSC、構成は1つです。

| インターフェース | 番号 | エンドポイント | 最大パケット |
| --- | --- | --- | --- |
| CDC Control | 0 | IN 0x81（通知） | 8 bytes |
| CDC Data | 1 | OUT 0x02 / IN 0x82 | 64 bytes |
| MSC SCSI / Bulk-Only | 2 | OUT 0x03 / IN 0x83 | 64 bytes |

- Manufacturer：`Cala Maclir`
- Product：`Cala's Pokecom BASIC`
- Serial：Picoの固有ID
- SCSI Inquiry：`CALA` / `CPB SD CARD` / `0.8`（8 / 16 / 4 bytes、空白で埋める）
- VID/PID：TinyUSB公式`cdc_msc`例と同じ開発用`CAFE:4003`。CPB専用に割り当てられたIDではありません。正式配布用USB製品としてのID確保は別途必要です。
- CDCのみの旧SDK IDから変わるため、初回の更新時はOSが新しいCOM番号を割り当てる場合があります。その後のメディア状態操作で再列挙は行いません。
- MSCのソフトウェア転送バッファは512 bytes。RAMディスクやSDキャッシュは追加しません。

## SDK stdioとの共存

Pico SDK **2.3.1**を使用します。`pico_enable_stdio_usb(... 1)`とUART stdioを維持します。
`pico_stdio_usb`が`tinyusb_device_unmarked`をリンクし、既存の`stdio_init_all()`からUSBを初期化します。
`tud_task`、CDC callback、stdio mutex、USB割り込みのバックグラウンド処理もSDKが管理します。
アプリケーションに別のUSB初期化や`task`ループは追加しません。

SDKの`tusb_config.h`をそのまま使い、`CFG_TUD_MSC=1`と`CFG_TUD_MSC_EP_BUFSIZE=512`を追加します。
`PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=0`でアプリケーションの固定記述子を使います。
SDKの初期化とIRQタスクを明示的に有効にします。
MSCとインターフェース番号が重なるSDKのvendor reset interfaceは無効にします。
SDKのCDC baud-rate reset機構は変更しません。BOOTSELによるUF2書き込みも従来どおりです。

`stdio_usb`、`stdio_usb_connected()`、`in_chars()`、`out_chars()`を維持し、
`serial_transfer.cpp`のUSB/UART選択処理、XMODEM/YMODEM本体は変更しません。
転送中やメニュー操作中も、SDKのIRQタスクがUSBを処理します。

## No Mediaと状態API

`usb_device::usb_connected()`はUSBがホストで構成済みかを返します。
ケーブルの有無やCDCのDTR／COMポートopen状態とは異なります。
MSCインターフェースは起動時から存在しますが、`usb_msc::active()`と`media_present()`は常にfalseです。
Stage 1の`set_media_present(true)`はfalseを返して拒否します。false指定は受理します。
いずれもUSBを切断・再初期化しません。

- TEST UNIT READY：失敗、NOT READY / MEDIUM NOT PRESENT（02/3A/00）。
- Capacity：0ブロック、ブロックサイズ512。SDKが固定しているTinyUSBは0ブロックを検出し、容量応答を失敗させます。
- READ10 / WRITE10 callback：バッファやSDへ触れず、-1とNo Media senseを返します。
- writable callback：false。TinyUSBがWRITE10を先に拒否する場合はwrite-protected senseになります。
- START STOP UNITのeject要求：`host_ejected`を記録します。USB CDCはそのままです。
- START／LOAD要求：No Mediaとして拒否。STOPのみではeject状態を変更しません。
- eject状態はUSB mount/unmount時に解除します。メインループとIRQの間はlock-free atomicで共有します。
- LUN 0以外、未対応SCSI、未対応power conditionは拒否します。

MSCソースはstorage、FatFs、ProgramStore、blockdeviceのヘッダー／APIを一切参照しません。
PicoCalc側では従来どおりSDカードをmountしたままDIR / LOAD / SAVEなどを利用できます。

## CIとメモリ

既存のhost tests、ASan/UBSan、ProgramStore fault tests、Pico 2 Wビルドとmemory reportを継続します。
追加テストではSDK付属の実際のTinyUSBヘッダーを使い、ハードウェア関数だけをstubにします。
記述子の構造・長さ・エンドポイント重複・文字列、No Media、eject、100回の接続状態遷移、
不正LUN、null／境界／巨大値のREAD/WRITE、Inquiryの書き込み境界を検証します。
USBホストや物理バスそのものをエミュレートするテストではありません。

CIは基準コミットも同じSDK・ARMツールチェーン・build numberでビルドします。
`memory-baseline-report.txt`、`memory-report.txt`、`memory-comparison.md`をUF2と同じartifactへ保存し、
`.data`、`.bss`、合計、`sizeof(Repl)`を比較します。Replのサイズが変化すると比較を失敗させます。
これはstatic SRAMの計測です。実行中のheap/stack最大使用量は実機計測が必要です。

## 実機確認手順

1. Pico 2 W自身のUSBとBOOTSELで、このPRの最新成功CIから取得したUF2を書き込みます。
2. Pico 2 W側のUSBをPCへ通常接続します。PicoCalc側UART用USB Type-Cとの取り違えに注意します。
3. WindowsのデバイスマネージャーでCDC COMポートとUSB Mass Storage Deviceを確認します。
   MSCはメディアなしです。エクスプローラーにドライブ文字が出ない場合もあります。
   SDカードの中身がPCに見える状態はStage 1の期待動作ではありません。
4. CDC COMポートをTera Termで開き、Enterで`BASIC>`を確認して`PRINT 1+2`を実行します。期待値は`3`です。
5. 接続したままMSCインターフェースが存在すること、PicoCalc側でDIR、テスト用BASのLOAD / SAVE / RUNが動くことを確認します。
6. 従来のUARTコンソールも確認します。CDCおよびUARTのXRECV / XSEND / YRECV / YSENDをテスト用ファイルで往復確認します。
7. ホストで可能ならMSCだけへSafe Ejectを要求し、CDCが利用可能なままであることを確認します。
   Windowsの「USB複合デバイス全体を取り外す」操作はCDCもOS側で外すため、MSC単独ejectとは異なります。
   `host_ejected()`はデバッガーから確認できます。Stage 1に状態表示UIは追加しません。
8. USBを抜き差しし、CDCとMSCが再認識され、再びBASIC入力できることを確認します。
9. CPU FULL/NORMAL/ECO、Wi-Fi（起動時OFF）、HTTP File Server、Program Storage、スクリーンショット、STANDBY、LCD・キーボード・Fキーも確認します。

macOSではシステム情報のUSBとシリアルポート、Linuxでは`lsusb -v`とCDC ACMポートで同様に確認できます。
No Mediaのため、USBディスクのフォーマットやファイルコピーは行いません。

## Stage 2への接続点

次段階ではmedia状態の背後にSD所有権管理を実装し、安全に準備できたときだけpresentを許可します。
READ10 / WRITE10へ512-byte blockdevice処理を接続するのは、その後です。
FatFsのunmount/remount、dirty program確認、ProgramStore suspend/resume・再scan、
HTTP／シリアル転送／スクリーンショットとの排他、取り外し・切断・ejectからの復旧は未実装です。
callbackから長時間のSD処理を直接呼ぶかどうかも、その段階で実行コンテキストとともに設計します。

## 照合した公式実装

- [Pico SDK 2.3.1 pico_stdio_usb](https://github.com/raspberrypi/pico-sdk/tree/2.3.1/src/rp2_common/pico_stdio_usb)
- [SDKが固定するTinyUSBのCDC + MSC例](https://github.com/hathach/tinyusb/blob/86ad6e56c1700e85f1c5678607a762cfe3aa2f47/examples/device/cdc_msc/src/usb_descriptors.c)
- [同じTinyUSBのMSCドライバー](https://github.com/hathach/tinyusb/blob/86ad6e56c1700e85f1c5678607a762cfe3aa2f47/src/class/msc/msc_device.c)

TinyUSB例のVID/PIDとエンドポイント構成に沿っています。記述子はTinyUSBのマクロで生成します。
