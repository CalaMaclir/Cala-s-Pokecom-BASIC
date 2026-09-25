# Cala's Pokecom BASIC Version 0.85 アーキテクチャ

## 対象と名称

対象はClockworkPi PicoCalc + Raspberry Pi Pico 2 Wです。製品名はCala's Pokecom BASIC（CPB）です。repository名、namespace、内部CMake target `retrominibasic_picocalc`には開発系譜の名称が残ります。外向けartifactは`CPokecombasic.uf2`、`CPokecombasic.elf`、`CPokecombasic-pico2w`です。

## 全体構造

```text
PicoCalc keyboard / LCD / SD / RTC-I2C / audio
                   |
              Platform layer
                   |
       REPL + Control Center + routing
       /          |           \
ProgramStore   BasicCompiler   SerialTransfer
 RAM / SD          |          USB / UART / SPP
 session            v          XMODEM / YMODEM
 continuity    CompiledProgram
                   |
                   VM
        graphics / PCG / audio service
```

## BASIC execution

`Repl`はline editor、direct mode、`LIST`／`RUN`／`LOAD`／`SAVE`、status、function keys、Control Centerを所有します。`BasicCompiler`はsourceをILとliteral／symbol／line mapを含む`CompiledProgram`へ変換し、`VM`が実行します。RUNとdirect statementは同じcompile workspaceを排他的に再利用します。

VMはcooperative BREAK poll時にbackground serviceも呼び、audio、network、Bluetoothを継続します。BREAKはprogramとbackground audioを停止しますが、通常のEND／STOPとruntime errorはaudioを自動停止しません。

## ProgramStore

`ProgramStore` abstractionはRAMとSD backendを同じREPL操作へ提供します。

| backend | lines | body characters | storage |
|---|---:|---:|---|
| RAM | 256 | 191 | fixed RAM entries |
| SD | 1,024 | 2,047 | source file + RAM index + reusable scratch |

行番号とその後のspaceはbody character数へ含めません。SD `Entry`はline number、file offset、full-line hash、body lengthを持ちます。2,080-byte scratchはSD backend ownerへheap allocationされ、scan、read、verify、compileで再利用されます。RAM用`ProgramLine`と`RamProgramStore`のlayoutは拡張していません。

SD→RAM切替は行数とbody lengthを事前検証します。191文字超過行があれば`LINE TOO LONG FOR RAM`でtransaction全体を拒否し、SD sourceを保持します。

## Storage Continuity

SD backendはcurrent editing sourceとdirty stateを`RMBASIC.SES`で追跡し、transactional temporary file、`RMBASIC.BAK`、recovery scanを組み合わせます。起動時はverified sessionだけを採用し、dirty sessionを復元した場合だけnoticeを表示します。fresh UNTITLEDとclean named sessionでは表示しません。

USB MSC開始前にSD ProgramStoreをsuspendし、hostへownershipを移します。return後は古いoffset／hashを破棄し、indexを再構築してから再開します。FirmwareとUSB hostが同時にFatFs／raw blockへアクセスしないownership modelです。

## Graphics and PCG

graphicsは物理320×320へvirtual coordinateを縦横比維持でmappingします。`SCREEN`、drawing、`PAINT`、`SAVE IMAGE`をplatform serviceへ渡します。

PCGはprintable ASCII 0x20～0x7eに8×8 glyphを定義します。mono dataは16 hex digits、indexed-color dataは128 hex digitsです。palette index 0はtransparentで、1～255は`GPALETTE`管理です。`GLOCATE`／`GPRINT` cursorは8 pixelずつ進みます。

## Audio

core audio engineは22,050 Hzで最大3 MML voiceとBEEPをmixします。WAV parserはPCM、mono／stereo、8／16 bit、11,025／22,050／44,100 Hzを受理します。PicoCalc側は6 DMA bufferで約139 msのheadroomを持ち、foreground VM serviceとPAINT cooperationで補給します。WAV終端はtail ramp後にidleへ移行します。Key Clickは同じPCM outputへ小さくmixし、program audioを停止しません。

## Bluetooth

BluetoothはCYW43439上のClassic RFCOMM SPP serverです。device nameは`CPB-PicoCalc`、同時clientは1つです。BLEはlinkしません。

Bluetooth Coreは固定ringとexclusive RX ownerを持ちます。

- RX ring：8192 bytes
- TX ring：2048 bytes
- owner：Console／Test Terminal／Transfer
- binary transfer：0x00～0xffをtext変換せず処理
- overflow counterと即時transfer failure
- transfer終了後は350 ms quiet windowのRX quarantine

## SerialTransfer and protocols

`SerialTransferRoute`はcommand input routeと分離されています。explicit routeはfallbackしません。AUTOはUSB／UART／Bluetooth command sourceを尊重し、local menuではconnected USB CDC、なければUART0です。

Bluetooth transferは2048-byte prefetchでbulk RXし、短いprotocol-control TXをdrainしてからreadへ進みます。XMODEMはsingle-file send／receiveです。YMODEM senderはsingle-file、receiverはfile loopを持つbatch receiveです。Block 0 sizeまでをcommitし、paddingを保存しません。duplicate Block 0、pre-data timeout ACK + `C`、partial-packet NAK／retry、final empty Block 0を処理します。

## USB composite

Pico 2 W native USBは固定composite descriptorです。

- Interface 0/1：CDC
- Interface 2：MSC
- Interface 3：Raspberry Pi reset vendor interface
- application identity：VID `0xcafe` / PID `0x4003`
- `bcdUSB 2.10`、Microsoft OS 2.0 descriptor

reset interfaceは`picotool -f`によるBOOTSEL移行を可能にします。MSC mediaのON/OFFでdevice全体をre-enumerateしません。

## Network

Wi-Fi、DHCP、DNS、NTP、HTTP File ServerはPico 2 WのCYW43／lwIPを使用します。Wi-Fiはsessionごとに手動ONです。File Serverはroot直下のfileをfixed bufferでstreamし、temporary fileから安全に置換します。

## Startup and power policy

- CPU：FULL 150 MHz
- Wi-Fi／File Server／Bluetooth／USB Storage：OFF
- Program Storage：保存設定（初期AUTO）
- `AUTORUN.BAS`を実行し、設定有効時は`AUTORUN.WAV`を再生
- CPU profiles：150／100／75 MHz、再起動時150 MHz

