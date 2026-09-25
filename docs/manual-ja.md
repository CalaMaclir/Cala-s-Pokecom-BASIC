# Cala's Pokecom BASIC Version 0.85 日本語マニュアル

対象機種：ClockworkPi PicoCalc + Raspberry Pi Pico 2 W（RP2350）  
略称：CPB
Copyright (C) 2026 Cala Maclir

> 本書はVersion 0.85を初めて使う利用者向けの正式ユーザーマニュアルです。過去Versionのmanualを読む必要はありません。

## 目次

1. はじめに
2. ファームウェアの導入と更新
3. 画面・keyboard・hotkey
4. BASICの基本操作
5. Program StorageとStorage Continuity
6. SD cardとfile
7. Control Center
8. USB Storage
9. Bluetooth Classic SPP
10. XMODEM／YMODEM
11. Wi-Fi／NTP／File Server
12. External RTC／I2C
13. Graphics
14. PCG
15. Audio
16. 設定file
17. Troubleshooting
18. Version historyとverification

## 1. はじめに

Cala's Pokecom BASICは、ClockworkPi PicoCalcとRaspberry Pi Pico 2 Wだけでprogramの作成、実行、保存、graphics、music、file transferを行えるstandalone BASIC環境です。

### 1.1 対応hardware

- ClockworkPi PicoCalc
- Raspberry Pi Pico 2 W（RP2350）
- FAT32形式のSD card（推奨）
- Wi-Fi使用時は2.4 GHz WLAN

現時点ではPico 2 W専用です。Pico、Pico W、Pico 2（非W）対応を示すものではありません。

### 1.2 Version 0.85の主な機能

- RAM 256×191／SD 1024×2047のProgram Storage
- dirty editing sessionの自動復元
- Graphics、PAINT、8×8 mono／indexed-color PCG
- 最大3 voice MML、BEEP、PCM WAV
- Bluetooth Classic SPP Consoleとfile transfer
- XMODEM send／receive、YMODEM single send・batch receive
- USB CDC + MSC + firmware reset interface
- Wi-Fi、NTP、browser File Server
- External PCF8563 RTCとI2C
- Firmware → Enter BOOTSEL／Reboot、Windows `flash-cpb.cmd`
- Key Click、F1～F10、Alt+S screenshot

### 1.3 起動時の状態

- CPU：FULL 150 MHz
- Wi-Fi：OFF
- Bluetooth：OFF
- Wi-Fi File Server：OFF
- USB Storage：OFF
- Program Storage：保存設定（初期AUTO）
- `AUTORUN.BAS`がある場合は自動実行
- Startup WAVがONなら、ProgramStore／AUTORUN.BAS処理後、最初のREADY prompt前に`AUTORUN.WAV`をbackground再生

dirty sessionを復元した起動では未保存編集を優先し、`AUTORUN.BAS`を実行しません。`[RESTORED EDITING SESSION]`はdirty sessionを実際に復元したときだけ表示します。fresh UNTITLEDとclean named sessionでは表示しません。

## 2. ファームウェアの導入と更新

### 2.1 配布物

GitHub Actions artifactは`CPokecombasic-pico2w`です。標準firmwareは次です。

```text
build/CPokecombasic.uf2
```

ELFを配布する場合の名称は`CPokecombasic.elf`です。内部targetの`retrominibasic_picocalc`はuser向けfirmware名ではありません。

### 2.2 初回導入

1. Pico 2 WをPicoCalcから外します。
2. Pico 2 W上のBOOTSELを押したまま、Pico 2 W側Micro-USBでPCへ接続します。
3. RPI-RP2 driveへ`CPokecombasic.uf2`をcopyします。
4. Pico 2 WをPicoCalcへ戻します。
5. FAT32のSD cardを装着して起動します。

PicoCalc本体USB Type-CとPico 2 W側Micro-USBの役割を混同しないでください。

| Port | 主な用途 |
|---|---|
| Pico 2 W Micro-USB | BOOTSEL、USB CDC、USB Storage、picotool update |
| PicoCalc USB Type-C | 電源／充電、CH340C経由UART0 |

### 2.3 Control Centerから更新準備

`Control Center → Firmware`に次があります。

- Enter BOOTSEL：確認後にRP2350 ROM USB bootloaderへ移行
- Reboot：確認後に通常再起動

USB Storage中はFirmware操作できません。host側で安全にEjectし、SD ownershipをCPBへ戻してください。

### 2.4 Windows `flash-cpb.cmd`

必要条件：

- `picotool.exe`がPATH上にある
- Pico 2 W側Micro-USBで接続
- USB Storageがactiveでない
- 標準配置では`flash-cpb.cmd`と`build\CPokecombasic.uf2`が同じartifact内にある

`flash-cpb.cmd`を実行すると、running CPBを`picotool -f`でBOOTSELへ移行し、UF2を書き込み・verifyします。UF2を引数指定またはdrag & dropすることもできます。

reset vendor interfaceを持たない古いfirmwareからVersion 0.85へ移行するときだけ、物理BOOTSEL操作が一度必要です。

## 3. 画面・keyboard・hotkey

### 3.1 通常画面

上3行はstatus、中央はconsole、最下段はfunction-key footerです。statusとfooterは通常のconsole scroll領域から分離されています。

| 行 | 表示例 |
|---|---|
| 1 | `CPB v0.85 NAME* SD:OK BAT:87%+` |
| 2 | `2026-09-25 17:30:00 WiFi:+ CAPS:A` |
| 3 | `CPU:ECO 75MHz CON:BOTH RUN:4.709s` |

`*`は編集済み、batteryの`+`は充電中です。WiFiは`-` OFF、`*` ON未接続、`+`接続済みです。

### 3.2 Function Keys

- F1～F5：通常
- Shiftを押している間：F6～F10
- assignment：Control Center → Quick Load Keys
- action：LOADまたはLOADしてRUN
- footer：現在のF1～F5／F6～F10を表示

### 3.3 Hotkeys

| 操作 | 機能 |
|---|---|
| HOME（Shift+Tab） | Control Center |
| Alt+S | screenshot |
| Power key／Alt+P | STANDBY（keyboard firmware互換fallbackを含む） |
| Alt+B | PicoCalc keyboard MCUのbattery表示用予約 |
| Alt+Space | keyboard MCUのkeyboard backlight cycle用予約 |

Alt+Space、Alt+BはCPB commandではありません。未実装の候補hotkeyをmanualへ追加しないでください。

### 3.4 Screenshot

Alt+SはLCD全体を24-bit BMPとしてSD cardへ保存します。filenameは`SCREEN0001.BMP`から空き番号を選び、既存fileを上書きしません。

REPL command：

```text
SCREENSHOT
SCREENSHOT "NAME"
```

BASIC statement：

```basic
SAVEIMAGE "NAME.BMP"
SAVE IMAGE "NAME.BMP"
SAVE IMAGE "NAME.BMP",X1,Y1,X2,Y2
```

## 4. BASICの基本操作

### 4.1 Program lineとDirect mode

行番号付き入力はprogramへ登録します。同じ行番号は置換、行番号だけなら削除です。

```basic
10 PRINT "HELLO, PICOCALC!"
20 FOR I=1 TO 10
30 PRINT I
40 NEXT I
```

行番号なしは即時実行するDirect modeです。

```text
BASIC> PRINT 2+3*4
14
```

### 4.2 REPL command

| Command | 機能 |
|---|---|
| `LIST` | program表示 |
| `RUN` | program実行 |
| `NEW` | programとdirect scalarをclearしcurrent filename解除 |
| `CLEAR` | direct scalarをclear |
| `LOAD "NAME"` | BASIC sourceをload |
| `SAVE` | current filenameへtransactional save |
| `SAVE "NAME"` | Save Asしてcurrent filename更新 |
| `FILES`／`DIR` | SD root一覧 |
| `SD`／`SD STATUS`／`SD REMOUNT` | SD状態／remount |
| `SCREENSHOT ["name"]` | BMP保存 |
| `XRECV "name"`／`XSEND "name"` | XMODEM |
| `YRECV`／`YSEND "name"` | YMODEM |
| `DATE`／`TIME`／`DATETIME` | clock表示・設定 |
| `SERIAL`／`CONSOLE` | USB CDC／UART console設定 |
| `PROFILE ...` | execution profile |
| `STANDBY` | RAM保持待機 |
| `MENU` | Control Center |
| `HELP` | command概要 |

`.BAS`を省略したLOAD／SAVEは拡張子を補います。引数なしSAVEは、最後に成功したLOADまたはSave Asのfileへ保存します。NEW後は`?FILENAME REQUIRED`となり、以前のfileを誤って上書きしません。

### 4.3 言語要素

- 数値／文字列variable、1D／2D array
- `LET`、`PRINT`、`INPUT`
- `IF THEN ELSE`
- `FOR NEXT STEP`
- `WHILE WEND`
- `DO LOOP UNTIL`
- `GOTO`、`GOSUB RETURN`
- `ON expression GOTO ...`、`ON expression GOSUB ...`
- `END`、`STOP`
- `REM`またはapostrophe comment
- arithmetic、comparison、logical、`MOD`、`^`

主なfunction：

`ABS`、`INT`、`VAL`、`STR$`、`LEN`、`CHR$`、`ASC`、`LEFT$`、`RIGHT$`、`MID$`、`INSTR`、`STRING$`、`SPC`、`TAB`、`SIN`、`COS`、`TAN`、`SQR`、`ATN`、`LOG`、`EXP`、`PI`、`RAD`、`DEG`、`SGN`、`MIN`、`MAX`、`CLAMP`、`RND`、`RNDI`、`TIMER`、`POINT`、`INKEY`、`I2CREAD`、`PLAYING`。

### 4.4 Hexadecimal literal

`&Hxxxx`をnumber literalとして使用できます。

```basic
10 A=&HFF
20 PRINT A
30 GPALETTE 2,&H00FF00
40 PRINT I2CREAD(&H51,&H02)
```

### 4.5 PAUSE、INKEY、BREAK、SLEEP

- `PAUSE`：画面を変えず通常keyを待つ
- `INKEY`：入力なし0、通常keyは数値code
- BREAK：programを停止し、background audioも停止
- `SLEEP milliseconds`：10 ms以下のchunkで待ち、BREAKとbackground serviceを継続

END／STOPによる通常終了とordinary runtime errorはbackground audioを自動停止しません。必要なら`PLAY STOP`または`WAVSTOP`を使います。

### 4.6 Runtime resource

| 項目 | 上限 |
|---|---:|
| 数値array | 合計4,096 cells |
| 文字列array | 合計512 cells |
| 文字列arrayの1 element | 127 characters |
| MML 1 voice string | 384 characters |

Direct modeのarrayはcommandごとにresetされます。

## 5. Program StorageとStorage Continuity

### 5.1 Backend capacity

| Backend | 最大行数 | 1行本文 |
|---|---:|---:|
| INTERNAL RAM | 256 | 191 characters |
| SD CARD | 1,024 | 2,047 characters |
| AUTO | active backendに従う | active backendに従う |

本文長にはline numberとその後のspaceを含めません。

- RAM 191：valid
- RAM 192以上：reject
- SD 2047：valid
- SD 2048以上：reject
- silent truncation：なし

### 5.2 SD long line

SD backendは191文字を超えるsource lineを`LOAD`、`LIST`、`RUN`、`SAVE`、session restoreできます。ただしPicoCalc keyboardのinteractive line editorとDirect modeは191文字のままです。

長い行はPCで編集し、次の方法でSDへ移します。

- USB Storage
- Wi-Fi File Server
- XMODEM／YMODEM
- その他のbinary-safe file transfer

191文字超過lineを含むSD programをINTERNAL RAMへ切り替えると`LINE TOO LONG FOR RAM`で安全に拒否し、SD sourceを破壊しません。256行超過では`PROGRAM TOO LARGE FOR RAM MODE`です。

### 5.3 Editing session

SD Program Storageはcurrent editing source、filename、dirty stateをsessionとして保持します。power cycle後にverified dirty sessionがあれば復元します。

技術file：

- `RMBASIC.SES`：session descriptor
- `RMBASIC.BAK`：設定backup
- temporary program file：transactional編集／保存用

通常利用では内部fileを操作しないでください。saveはtemporary fileを正常に閉じて検証した後だけfinal filenameへ置き換えます。power lossやcrash後はvalid candidateをscanします。

### 5.4 USB Storage return

USB Storage中はhostがSD cardを所有し、CPB firmwareはFatFsへアクセスしません。CPBへreturnした後は、USB公開前のoffset／hashを信用せずProgramStore indexを再構築します。

## 6. SD cardとfile

SD cardはFAT32を推奨します。mount failure時に自動formatしません。BASIC sourceと一般fileはroot directoryへ置きます。

Files menu：

- Enter：LOAD
- R：LOAD and RUN
- F1～F10：quick key assignment
- Esc：back

current ProgramStoreが使用中のsourceは、File Server、XRECV、YRECVによるoverwrite／deleteから保護されます。

## 7. Control Center

空の`BASIC>` promptでHOME（Shift+Tab）を押します。

1. Files
2. Save Program
3. Save Program As...
4. Quick Load Keys
5. Display
6. Console
7. Date / Time
8. Audio
9. Wireless LAN
10. Bluetooth
11. Wi-Fi File Server
12. File Transfer
13. USB Storage
14. SD Card
15. Firmware
16. Power / CPU
17. System Information
18. Program Storage
19. Exit

### 7.1 Display／Console

Displayではstatus、LCD backlight、status theme、console foreground／backgroundを設定します。

Console mode：

- LCD ONLY
- LCD + SERIAL
- SERIAL ONLY

USB CDCとUART0はserial inputとして扱います。Bluetooth Consoleは別のON／OFFです。

### 7.2 Audio

- Volume：0～100、10刻みcycle
- Startup WAV：ON／OFF
- Key Click：OFF／SOFT／CLASSIC／SHARP

Key Click defaultはCLASSICです。物理PicoCalc keyのREPL／Control Center操作だけが鳴ります。program実行中のINKEY／PAUSE／BREAK、USB serial、Bluetooth remote consoleは鳴りません。

### 7.3 Power／CPU

| Profile | Clock |
|---|---:|
| FULL | 150 MHz |
| NORMAL | 100 MHz |
| ECO | 75 MHz |

50 MHzは対応値ではありません。CPU profileはsession-onlyで、再起動時に150 MHzへ戻ります。

## 8. USB Storage

Pico 2 W側Micro-USBからPicoCalcのSD cardをUSB Mass StorageとしてPCへ公開します。

### 8.1 開始

1. Pico 2 W側Micro-USBをPCへ接続
2. Control Center → USB Storage
3. Enable USB Storage
4. PC側でfile操作

この間、SD ownerはUSB HOSTです。CPB側のDIR、LOAD、SAVE、SD-backed LIST／RUN／編集、transfer、screenshot、File Server、STANDBYは制限されます。

### 8.2 終了

通常手順：

1. PC側でUSB diskを安全にEject
2. PicoCalcでReturn SD to CPB
3. remountとProgramStore index rebuild完了を待つ

Force Disconnectはhostのpending writeでdataを失う可能性がある緊急手段です。確認初期値はCancelです。Firmware update前は必ずownershipをCPBへ戻します。

## 9. Bluetooth Classic SPP

### 9.1 方式

- Classic Bluetooth SPP／RFCOMM
- device name：`CPB-PicoCalc`
- 同時client：1
- BLEではない
- Bluetoothは起動時OFF

Control Center → Bluetooth：

- Enable／Disable Bluetooth
- Console ON／OFF
- Test Terminal
- status、device、RX／TX overflow、last status

File TransferはControl Centerの独立menuです。

### 9.2 Bluetooth Console

Console ONにするとPC terminalからBASIC REPLを操作できます。LCDと既存USB CDC／UART0 ConsoleModeは維持されます。1行の入力は最初に入力したsourceが所有します。

Test TerminalはSPP connection、echo、test messageの確認用です。Console／Test Terminal／TransferはRXを同時使用しません。

### 9.3 ConsoleとFile Transferの違い

Bluetooth file transferにConsole ONは不要です。必要なのは：

- Bluetooth ON
- SPP link connected
- Test Terminal／別transferがRXを所有していない

transfer中はbinary 0x00～0xffをCR/LF conversion、ANSI decode、echo、NUL filter、Ctrl+C conversionなしで扱います。

## 10. XMODEM／YMODEM

### 10.1 Transport selector

Control Center → File Transferの先頭で選択します。

- AUTO
- USB CDC
- UART0
- Bluetooth SPP

LEFT／RIGHTまたはENTERでcycleします。selectionはmenuを閉じるまでで、`RMBASIC.CFG`へ保存しません。

AUTO resolution：

1. commandをUSBから入力した場合はUSB CDC
2. UARTから入力した場合はUART0
3. Bluetooth Consoleから入力した場合はBluetooth SPP
4. PicoCalc本体から開始した場合はconnected USB CDC、なければUART0

Bluetoothが接続されただけで、local AUTO transferがBluetoothへ切り替わることはありません。explicit routeは失敗時に別routeへfallbackしません。

### 10.2 XMODEM

対応：

- receive／send
- USB CDC／UART0／Bluetooth SPP
- single-file
- CRC-16/XMODEM
- 128-byte block

```text
XRECV "TEST.BAS"
XSEND "TEST.BAS"
```

XMODEM headerにはfilenameとexact sizeがありません。sendした最終blockの`0x1a` paddingがPC側file末尾に残る場合があります。exact-sizeが必要ならYMODEMを使います。

### 10.3 YMODEM

対応：

- receive：single-file、multi-file batch
- send：single-file
- 128-byte SOH／1,024-byte STX
- header filename／decimal file size
- exact-size commit
- final empty Block 0

```text
YRECV
YSEND "TEST.BAS"
```

PC→PicoCalc receiveではBlock 0のsizeだけを保存し、末尾paddingを保存しません。batchでは各fileを個別transactionとしてcommitし、後続fileのcancel／error時も完了済みfileは保持します。不完全なcurrent fileは残しません。

### 10.4 Bluetooth final implementation

開発者向け参考値：

- RX ring：8192 bytes
- TX ring：2048 bytes
- transfer prefetch：2048 bytes
- bulk RX、overflow即時検出
- short protocol-control TX drain
- post-transfer RX quarantine
- duplicate Block 0 recovery
- pre-data timeout ACK + C recovery
- partial packet timeout NAK／retry

診断中の`[YDBG]`は最終版から削除済みです。

## 11. Wi-Fi／NTP／File Server

### 11.1 Wi-FiとNTP

Wireless LANから明示的にONにします。保存済みSSIDがあっても起動時に自動接続しません。

- SSID scan／connect
- IPv4表示
- NTP sync
- timezone：-720～+840 minutes
- NTP server設定
- auto RTC update設定

Wi-Fi passwordは`RMBASIC.CFG`へ平文保存されます。

### 11.2 Wi-Fi File Server

browserだけでSD rootのlist、upload、download、deleteを行えます。

1. Wi-FiをON／connect
2. Wi-Fi File Server → Start
3. 画面のtoken付きURLをbrowserで開く

session tokenはstartごとに変わり、SDへ保存しません。uploadはtemporary fileへstreamし、Content-Length分を正常に書き終えた場合だけreplaceします。同時1 client／1 operationを基本とします。

`LOADURL`と`RUNURL`は未実装です。

## 12. External RTC／I2C

### 12.1 RTC

標準PicoCalcにはRTCがありません。optional PCF8563をExternal I2Cへ接続できます。

Control Center → Date / Time → RTC settings：

- Source：AUTO／EXTERNAL／INTERNAL／OFF
- Type：PCF8563
- target address：0x08～0x77
- Probe RTC
- Set date & time
- External SDA：GP4
- External SCL：GP5
- speed：100 kHz

### 12.2 I2C syntax

```basic
I2C SCAN
A=I2CREAD(address,register)
I2CWRITE address,register,value
```

- 7-bit address：0x08～0x77
- register：0～255
- write value：0～255

例：

```basic
10 I2C SCAN
20 S=I2CREAD(&H51,&H02)
30 PRINT S
40 I2CWRITE &H20,&H01,&HFF
```

## 13. Graphics

物理LCDは320×320です。`SCREEN w,h`はvirtual coordinateを縦横比維持で中央mappingします。`SCREEN`引数なしは640×480 virtual screenです。virtual coordinateと物理pixelを混同しないでください。

| Syntax | 機能 |
|---|---|
| `SCREEN [w,h]` | virtual screen |
| `CLS` | black clear |
| `COLOR palette` | built-in color index |
| `COLOR r,g,b` | RGB 0～255 |
| `COLORHSV h,s,v` | h degree、s/v 0～1 |
| `PSET x,y[,color]` | pixel |
| `LINE x1,y1,x2,y2[,color]` | line |
| `LINE x,y[,color]` | current pointからline |
| `CIRCLE x,y,r[,color]` | circle |
| `BOX x1,y1,x2,y2[,filled[,color]]` | rectangle |
| `PAINT x,y[,color]` | flood fill |
| `POINT(x,y)` | non-blackならtrue |
| `FLUSH` | display flush |
| `GLOCATE x,y` | graphics text cursor |
| `GPRINT expr[,expr...]` | graphics text |
| `SAVEIMAGE ...`／`SAVE IMAGE ...` | BMP保存 |

例：

```basic
10 SCREEN 320,320
20 CLS
30 COLOR 255,255,255
40 LINE 0,0,319,319
50 CIRCLE 160,160,80
60 FLUSH
70 PAUSE
```

## 14. PCG

### 14.1 Character range

`GDEF`対象はprintable ASCII 0x20～0x7eです。1 character単位で8×8 pixelを定義します。

### 14.2 Mono PCG

16 hex digits＝8 rows × 1 byteです。bit 7がleft pixelです。set bitはcurrent COLOR、clear bitはbackgroundを描きます。

```basic
10 GDEF "A"="183C66667E666600"
20 COLOR 255,255,0
30 GLOCATE 40,40
40 GPRINT "A"
```

`GDEF "A"=""`はAだけをbuilt-inへ戻し、`GDEF CLEAR`は全定義をclearします。

### 14.3 Indexed-color PCG

128 hex digits＝64 pixels × 1 palette-index byteです。

```basic
10 GPALETTE RESET
20 GPALETTE 8,18,38,84
30 GPALETTE 9,&H1ED6FF
40 GDEF "A"="..."
```

`GPALETTE index,r,g,b`または`GPALETTE index,rgb24`でindex 1～255を設定します。index 0はtransparentで、設定対象ではありません。`GPALETTE RESET`はdefault paletteへ戻します。

### 14.4 16×16 composite character

8×8が小さい場合は4 characterを2×2配置します。

```basic
100 GLOCATE X,Y:GPRINT "AB"
110 GLOCATE X,Y+8:GPRINT "CD"
```

3 frame animationは`AB/CD`、`EF/GH`、`IJ/KL`のように定義し、frameごとに描画します。

```basic
200 M=F MOD 3
210 ON M+1 GOSUB 8100,8200,8300
220 FLUSH:SLEEP 40
8100 GLOCATE X,Y:GPRINT "AB":GLOCATE X,Y+8:GPRINT "CD":RETURN
8200 GLOCATE X,Y:GPRINT "EF":GLOCATE X,Y+8:GPRINT "GH":RETURN
8300 GLOCATE X,Y:GPRINT "IJ":GLOCATE X,Y+8:GPRINT "KL":RETURN
```

完全な例は`examples/CPB_V085_LONG_LINE_MEGADEMO.BAS`です。

## 15. Audio

### 15.1 BEEP

```basic
BEEP frequency_hz,duration_ms
```

- frequency：20～20,000 Hz
- duration：1～60,000 ms

### 15.2 PLAY／MML

```basic
PLAY voice1$[,voice2$[,voice3$]]
PLAY PAUSE
PLAY RESUME
PLAY WAIT
PLAY STOP
N=PLAYING()
```

最大3 voiceです。PLAYはbackgroundで開始し、graphics等と同時進行します。

MML：

| Token | Range／meaning |
|---|---|
| `Tn` | tempo 32～400 |
| `On` | octave 0～8 |
| `Ln` | default length 1,2,4,8,16,32 |
| `Vn` | level 0～15 |
| `C D E F G A B` | note |
| `R` | rest |
| `#`／`+` | sharp |
| `-` | flat |
| `<`／`>` | octave down／up |
| note後のnumber | length |
| `.` | dotted |

例：

```basic
10 PLAY "T120O5L8V12 CDEFGAB>C","T120O4L4V8 CEGC","T120O3L2V8 C<G>C"
20 REM GRAPHICS CONTINUES HERE
30 PLAY WAIT
```

### 15.3 WAV

```basic
WAVPLAY "DEMO.WAV"
WAVPAUSE
WAVRESUME
WAVSTOP
```

対応format：

- RIFF/WAVE PCM（format 1）
- monoまたはstereo
- 8 bitまたは16 bit
- 11,025／22,050／44,100 Hz
- block alignmentがformatと一致

WAVもbackground playbackです。CPU負荷と大量LCD transferにはhardware上限がありますが、Version 0.85はVM background service、6 DMA buffers、PAINT cooperationで途切れを改善しています。

### 15.4 終了条件

- BREAK：BASIC executionとbackground audioを停止
- END／STOP：program終了、audioは継続
- ordinary runtime error：audioは継続
- `PLAY STOP`／`WAVSTOP`：明示停止

## 16. 設定file

`RMBASIC.CFG`の現行設定：

| Section | Key | 値 |
|---|---|---|
| ui | `program_storage` | AUTO／SD／RAM |
| ui | `status` | on／off |
| ui | `backlight` | 16～255 |
| ui | `theme` | 0～2 |
| ui | `console_fg`／`console_bg` | RRGGBB |
| ui | `console` | lcd／both／serial |
| rtc | `rtc_source` | AUTO／EXTERNAL／INTERNAL／OFF |
| rtc | `rtc_address` | 0x08～0x77 |
| audio | `audio_volume` | 0～100 |
| audio | `key_click` | OFF／SOFT／CLASSIC／SHARP |
| audio | `startup_wav` | on／off |
| wifi | `wifi_enabled` | 読み込み時もsession startはOFF |
| wifi | `wifi_ssid`／`wifi_password` | credential |
| wifi | `wifi_auto_rtc` | on／off |
| wifi | `wifi_timezone_minutes` | -720～840 |
| wifi | `wifi_ntp_server` | hostname |
| fkeys | `F1`～`F10` | filename,LOAD／RUN |

CPU clockとBluetooth ON/OFFは永続化しません。設定save前のvalid fileは`RMBASIC.BAK`へ保護されます。

## 17. Troubleshooting

### 17.1 SD NOT AVAILABLE

- FAT32か確認
- cardを挿し直す
- SD Card → Remount
- Last statusを確認
- USB HOST ownershipでないか確認

### 17.2 SD LINE TOO LONG

SD source bodyが2,048文字以上です。line numberとspaceを除いたbodyを2,047文字以内へ修正してください。切り詰めは行いません。

### 17.3 LINE TOO LONG FOR RAM

SD sourceに192文字以上のbodyがあります。SD sourceは保持されています。SD CARDのまま使用するか、long lineを191文字以内へ分割してください。

### 17.4 USB Storageを戻せない

hostでdiskを安全にEjectしてからReturn SD to CPBを再試行します。Force Disconnectは最後の手段です。

### 17.5 Bluetooth file transferが開始しない

- Bluetooth ON
- PCとのSPP接続済み
- Test Terminalを閉じる
- File TransferでBluetooth SPPをexplicit選択
- Bluetooth Console ONは不要
- PC terminal側で同じprotocol方向を選択

### 17.6 YMODEM TIMEOUT／PROTOCOL ERROR

- sender／receiverの開始順を確認
- XMODEMとYMODEMを混同しない
- transport selectorを確認
- Bluetooth diagnosticsのRX Overflowを確認
- error後は350 ms以上linkをquietにして再試行

### 17.7 XMODEM末尾の`^Z`

XMODEMはexact sizeを通知しません。PC側にpaddingが残る場合があります。YMODEMを使用してください。

### 17.8 WAVが再生できない

PCM format、channel、bit depth、sample rateを対応範囲へ変換します。圧縮WAV、24 bit、48 kHz等は対応しません。

## 18. Version historyとverification

### 18.1 History

- v0.8：初公開
- v0.81：PAUSE／INKEY／runtime input／BREAK／current-file SAVE
- v0.82：External RTC／I2C／PCG／`&H`
- v0.83：BEEP／3 voice MML／WAV
- v0.84：Storage Continuity
- v0.85：Bluetooth SPP、Bluetooth Console、Bluetooth XMODEM／YMODEM、YMODEM batch receive、Firmware controls、`flash-cpb.cmd`、audio/display refinement、Key Click、SD 2047-character line、`CPokecombasic` artifact naming

### 18.2 Version 0.85実機確認

Bluetooth SPP PC→PicoCalc YMODEM：

- 1-byte single file：PASS
- 4040-byte single file：PASS
- 2-file batch：PASS
- 5-file batch：PASS
- RX overflow delta：0
- `TRANSFER COMPLETE`

Firmware：

- Enter BOOTSEL：PASS
- Reboot：PASS
- `flash-cpb.cmd`：PASS

Main GitHub Actions #389：SUCCESS。

### 18.3 Examples

- `CPB_V085_LONG_LINE_MEGADEMO.BAS`：exact 2047 body、3 voice、graphics、indexed PCG、16×16 composite、animation
- `CPB_BACH_MEGADEMO.BAS`：music／graphics／PCG
- `Lissajous_Gallery.BAS`：graphics
- `mandel_graphics.bas`／`julia_graphics.bas`：fractal
- `3dhat.bas`：3D
- `mandel_text.bas`：benchmark

---

Cala's Pokecom BASICの製品名はCPBです。RetroMiniBASICという名称はrepository／内部target／歴史的説明にのみ残ります。
