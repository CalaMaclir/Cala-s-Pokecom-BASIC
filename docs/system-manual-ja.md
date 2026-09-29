# Cala's Pokecom BASIC System
## Version 0.91
## System Manual / システムマニュアル

本書は ClockworkPi PicoCalc 上の Cala's Pokecom BASIC（CPB）v0.91 の操作、設定、保存、通信、診断を説明します。BASIC 文法は `programming-reference-ja.md` を参照してください。

## 1. System Overview

CPB は PicoCalc 単体で BASIC プログラムを作成・実行・保存できる環境です。LCD、キーボード、SD、USB、Wi-Fi、Bluetooth、音声、External RTC / I2C を一つのシステムにまとめています。

起動後は `BASIC>` prompt が表示されます。空の prompt で HOME（Shift+Tab）を押すと Control Center、`EDIT` と入力すると Full-Screen BASIC Editor が開きます。

## 2. Keyboardと共通操作

| 操作 | 機能 |
|---|---|
| Enter | 決定、BASIC行またはdirect commandの入力 |
| Esc | 戻る、キャンセル、実行中programのBREAK |
| Up / Down | 1項目移動、promptではcommand history |
| Shift+Up / Shift+Down | 対応一覧で1画面移動 |
| HOME（Shift+Tab） | 空のpromptでControl Center |
| F1〜F10 | promptではQuick Load Keys、Editorでは編集command |
| Alt+S | screenshotをSDへ保存 |

### Command history

`BASIC>` では直近24件を session 中だけ保持します。空行と連続する重複は登録されません。Up で過去、Down で新しい履歴へ移動し、履歴を開く前に入力していた draft へ戻れます。再起動すると消去されます。

## 3. Control Center

空の`BASIC>` promptでHOME（Shift+Tab）を押すとControl Centerを開きます。v0.91ではカテゴリ見出しと項目を同一画面に展開し、メニューlabelの`...`は使用しません。

```text
Files
Editor
Program
    Save Program
    Save Program As
    Quick Load Keys
    Program Storage
Storage
    SD Card
    USB Storage
Display & Audio
    Display
    Audio
Network
    Wireless LAN
    Wi-Fi File Server
    Bluetooth
Serial
    Console
    Serial Config
    File Transfer
System
    Date / Time
    Power / CPU
    Board LED
    Firmware
Diagnostics
    System Information
    PSRAM Diagnostics
Exit
```

表示はrow 37まで使用し、row 38を空行、row 39をFunction Key barとして確保します。項目が画面内に収まるため、通常は一覧全体を一度に確認できます。

- Up / Down: selectable itemを移動
- Shift+Up / Shift+Down: page移動が必要な一覧で使用
- Enter: open / execute
- Esc / HOME: back / exit

Network、Serial、Diagnosticsを含む各カテゴリは階層化され、たとえば`Serial -> Serial Config`、`Serial -> File Transfer`のように移動します。

## 4. BASIC prompt

`BASIC>`ではDirect modeの実行、line number付きprogram編集、REPL commandを行います。interactive入力は従来どおり191文字境界です。2,047文字logical lineはFull-Screen Editorまたは外部file経由で扱います。

主なcommand:

- `LIST`: current programを表示。BREAK／Esc／Ctrl-Cで途中停止でき、`[LIST BREAK]`の後にpromptへ戻る
- `RUN`: current programを実行。処理開始時に`RUN...`を表示する
- `LOAD "name"`: 読み込み開始時に`LOADING... (filename)`、成功後に`LOADED filename`を表示する
- `EDIT`: Full-Screen BASIC Editor
- `NEW`: programとDirect stateをclear
- `CLEAR`: Direct scalarだけをclear

長いprogramではLOADや初回RUNのcompileに時間がかかる場合があるため、v0.91では進捗表示を先に出して操作を受け付けたことが分かるようにしています。

## 5. Program Storage

Program StorageにはAUTO、SD CARD、INTERNAL RAMがあります。

| backend | 最大行数 | 1行本文 | 特徴 |
|---|---:|---:|---|
| INTERNAL RAM / PSRAM | 1,024 | 2,047 | SD不要、高速編集、電源断で消える |
| INTERNAL RAM / SRAM fallback | 256 | 191 | PSRAMなし／claim不可時の互換mode |
| SD CARD | 1,024 | 2,047 | 永続、transactional working source |

PSRAM利用時のINTERNALはSDと同じsource容量です。約2 MiBのtext slot領域とcompact indexを使い、行挿入・削除時は本文全体ではなくindex metadataを移動します。

SDの大規模programをINTERNALへ切り替える場合も、1,024行・2,047文字の範囲なら内容を保持したまま移行できます。実機では1,024行stress programと2,047文字行をSDからINTERNALへ移し、編集・RUNを確認しています。

PSRAMが利用できなければ従来の256行×191文字SRAM backendへfallbackします。System Informationで現在のcapacityとPSRAM allocationを確認できます。

Direct modeと通常promptの入力bufferは191文字のままです。source backendの2,047文字対応とは別の制限です。

## 6. Storage Continuity

編集中programの状態をSDへ安全に退避し、異常終了や再起動後の復元候補として提示します。保存は temporary file と rename を使い、途中状態を current session として採用しにくい設計です。

復元候補が表示されたら内容と current file を確認し、Restore または Discard を選びます。USB Storage を開始する前には編集中内容を明示的に保存してください。

## 7. Files

Files には2つのmodeがあります。

| Mode | 用途 | 主な操作 |
|---|---|---|
| PROGRAMS | BASIC program中心 | Enter LOAD、R RUN、E EDIT、N RENAME、Del DELETE、I INFO、F REFRESH |
| DIRECTORY | root上の全対象file | P PLAY/STOP、N RENAME、Del DELETE、I INFO、F REFRESH |

Left / Right でmodeを切り替えます。v0.91 は root-only で、directory navigation は行いません。

### ファイル名

- 最大79文字
- ASCII の内部空白を許可
- 先頭・末尾の空白とドットは禁止
- `..`、`/`、`\\`、制御文字、非ASCII文字は禁止
- program名は必要に応じて `.BAS` を補完

Rename と Delete は current program への影響を考慮して処理されます。Delete は確認画面を表示します。大文字・小文字だけを変える rename は FAT の条件により制限があります。

## 8. Full-Screen BASIC Editor

起動方法:

- `BASIC>` で `EDIT`
- `Control Center → Editor`
- Files PROGRAMS で対象を選び `E`

Editor は program 全体の複製を作らず、現在行用の約2 KiB buffer と ProgramStore を使用します。長い logical line は 320×320 LCD の幅に合わせて折り返されます。行番号prefixを除く本文は1画面41文字幅が目安です。

### Editorキー

| キー | 機能 |
|---|---|
| Arrow keys | 文字・visual row単位の移動 |
| Shift+Up / Shift+Down | 1画面移動 |
| Home / End | logical lineの先頭 / 末尾 |
| Backspace / Delete | 文字削除 |
| F1 | Save。untitledならSave As |
| F2 | Find |
| F3 | Next match |
| F4 | Undo |
| F5 | Insert Line |
| F6 | Save As |
| F7 | Goto line |
| F8 | Previous match |
| F9 | Redo |
| F10 | Delete Line（確認あり） |
| Esc | 終了。未保存なら確認 |

Enter は logical line を分割しません。新しい行を追加する場合は F5 を使います。

### Undo / Redo

PicoCalc PSRAM が利用できる場合、Editor は128 KiBを履歴用に取得し、おおむね31件の coalesced Undo と31件の Redo を保持します。連続入力はまとめられる場合があります。Editor Historyはエディタを閉じると破棄されます。program本体の保存先はProgram Storage backendに従い、INTERNAL PSRAM modeではprogram source自体もPSRAM ProgramStoreに保持されます。

PSRAMがない場合も編集と保存は利用できます。F4 / F9 は無効で、状態行に理由が表示されます。

### 性能

v0.91 はカーソルだけの移動を部分再描画し、key repeat中の全画面描画を抑えます。SD viewport は1回の lease / open / close でまとめて読み出します。性能測定用 UF2 は通常利用向けではありません。

## 9. Quick Load Keys

F1〜F10 に `.BAS` と LOAD / RUN を割り当てられます。prompt で押すと登録内容を実行します。Editor が開いている間は Editor command が優先されます。

設定は `RMBASIC.CFG` の `[fkeys]` に保存されます。

## 10. Display

Backlight、Theme、Console foreground / background、status表示を設定します。Backlight は安全な下限を持ちます。screenshot は Alt+S でSDへ保存します。

## 11. Console

| Mode | 出力先 |
|---|---|
| LCD | PicoCalc LCD |
| SERIAL | USB CDC / serial console |
| BOTH | LCDとserialの両方 |

入力元と出力先は転送処理にも関係します。File Transfer の AUTO は、commandを開始した入力元を優先します。

## 12. Date / TimeとRTC

RTC source は AUTO、EXTERNAL、INTERNAL、OFF から選択します。External RTC はI2C経由で扱い、addressも設定できます。Wi-Fiが接続されている場合はNTPから時刻を取得でき、timezone offsetは分単位で指定します。

RTC hardwareが見つからない場合は `RTC N/A` と表示されます。配線、address、source設定を確認してください。

## 13. Audio Settings

v0.91 のAudioは3層です。

| 設定 | 範囲 / 既定 | 対象 |
|---|---|---|
| Master | OFF、10〜100 / 70 | BEEP、PLAY、WAV、MP3、診断音 |
| WAV/MP3 | 10〜100 / 50 | file playback |
| PLAY | OFF、10〜100 / 100 | MML PLAY |

WAV / MP3 の出力ceilingは PLAY の2倍です。Masterはすべてに共通して最後に適用されます。

`Key Click` は OFF / SOFT / CLASSIC / SHARP、`Startup WAV` は起動時の `STARTUP.WAV` を制御します。

### WAV / MP3 playback

`WAVPLAY "filename"` は拡張子だけでなく内容を判定します。PCM WAV と MP3（44.1 / 48 kHz、mono / stereo）をstreaming再生します。Files DIRECTORYでは対象を選び `P` で再生・停止できます。

MP3開始時は必要に応じて200 MHzを自動要求し、停止後に元のclockへ戻ります。

## 14. Wireless LAN

2.4 GHz WLANへ接続します。SSIDとpasswordは設定へ保存できますが、接続状態は session-only で、起動時はOFFです。

接続後はNTP、Wi-Fi File Serverを利用できます。接続できない場合はSSID、password、2.4 GHz、電波状態を確認します。

## 15. Bluetooth Keyboard

Bluetooth Classic HID / BLE HID Keyboard に対応します。Pair、Reconnect、Disconnect、Forget、JIS / US layoutを選択できます。Paired DevicesにはKeyboardとして登録された機器だけを表示します。

v0.91のBluetoothはkeyboard入力用途です。旧版のClassic SPP file transferとは役割が異なります。

## 16. Wi-Fi File Server

Wi-Fi接続後、HTTP File Serverを開始すると同一LANのbrowserからSD rootへupload、download、deleteできます。画面に表示されるURLへ接続してください。

利用後はserverを停止します。公開ネットワークでは使用しないでください。

## 17. File Transfer

File TransferのtransportはAUTO、USB CDC、UART0です。Bluetooth SPP/RFCOMMはv0.89以降の現行構成には含まれません。

| Protocol | Send | Receive |
|---|---|---|
| XMODEM | single-file | single-file |
| YMODEM | single-file | single / multi-file batch |

v0.91 Stage 1ではtransfer hot pathを次のように最適化しています。

- YMODEM RX: header／payload／CRCをbulk exact-read
- YMODEM TX: 133／1029-byte packetをcoalesceしてwrite
- USB CDC: bulk read
- UART RX ring: 4096-byte aligned SRAM buffer
- UART baud: 115200／230400／460800／921600
- UART RX mode: IRQまたはRP2350 RX-only DMA
- cancel polling: 25 ms

921600 bps・YMODEM DMAの実機測定では、PC→PicoCalc約33 KB/s、PicoCalc→PC約62 KB/sでした。IRQとDMAのthroughput差は小さく、DMAは主としてRX時のCPU負荷低減を目的とします。

YMODEMはclassic stop-and-wait protocolなので、UARTそのものの理論速度よりACK、storage、sender gap等が律速になることがあります。

## 18. USB Storage

SDカードをPCへUSB Mass Storageとして公開します。開始前にprogramと設定を保存してください。MSC中はCPBからSDへ通常アクセスできず、CPU profile切替も拒否されます。

終了後はPC側で安全な取り外しを行い、CPBメニューから停止してSDを再mountします。

## 19. Firmware

`Enter BOOTSEL` は次回書き込み用の `RPI-RP2` modeへ移行します。対応前の古いfirmwareでは物理BOOTSELが必要です。通常版とeditor performance計測版を取り違えないでください。

## 20. Power / CPU

| Profile | Clock | 用途 |
|---|---:|---|
| ECO | 75 MHz | 低消費電力 |
| NORMAL | 100 MHz | 軽量処理 |
| FULL | 150 MHz | 起動時既定、通常利用 |
| EXP | 200 MHz | 高負荷、MP3、実験的 |

v0.91 はprofile切替時に必要ならCYW43関連のWi-Fi / Bluetooth / Board LED serviceを停止・再初期化します。利用者が先に手動停止する必要はありません。USB Mass Storage中、PSRAM診断や競合処理中は安全のため切替を拒否します。

CPU profileは保存されず、再起動時は必ず150 MHzへ戻ります。

## 21. Board LED

Pico 2 W のLEDはCYW43 deviceを共有します。OFF、ON、HEARTBEATを選べます。HEARTBEATは約1秒ごとに120 ms点灯します。CPU profile切替後も必要に応じて復旧します。

## 22. System Information

表示項目:

- CPB version / Build
- CPU clock、uptime、last RUN time
- Current program、dirty marker
- Program lines / current line capacity
- Console、SD、battery、Wi-Fi、RTC
- PicoCalc PSRAM容量、PIO state machine、bus clock
- PSRAM aggregate allocation
- Editor History / Program Store / SD Cache
- Direct State / Compiled Cache
- SD cache hit / miss
- Compiled Cache VALID / EMPTY、hit / miss
- DirectState PSRAM / SRAM fallback state

v0.91のPSRAMは単一ownerではなくmulti-client allocatorで管理されます。INTERNAL ProgramStoreは通常約2 MiBをclaimし、Editor History、SD Cache、DirectState、Compiled Cacheと同時利用できます。VMのhot opcode実行は内部SRAMで行います。

## 23. PSRAM Diagnostics

| Test | 範囲 | 用途 |
|---|---|---|
| Quick | 1 MiB | 短時間の基本確認 |
| Full | 検出全容量 | 全領域の確認 |
| 3-pass stress | 検出全容量×3 | 安定性確認 |

診断はPSRAM内容を上書きするため、persistent clientが存在する場合は安全のため拒否されます。INTERNAL ProgramStoreがPSRAMを使用している場合やDirectStateが保持されている場合は、必要なprogramを保存してstorage modeを変更し、Direct variableは`CLEAR`してから実行します。

SD read cacheとCompiled Cacheはdisposable clientなので、診断前に解放できます。起動時probeは末尾16 bytesだけを使用し、その領域はallocatorから予約されています。

## 24. RMBASIC.CFG

設定はSD rootの`RMBASIC.CFG`へ保存されます。主なkeyは次のとおりです。

```ini
[ui]
program_storage=AUTO
status=on
backlight=255
board_led=HEARTBEAT
theme=0
console_fg=FFFFFF
console_bg=000000
console=BOTH

[rtc]
rtc_source=AUTO
rtc_address=0x51

[audio]
audio_volume=70
wav_volume=50
play_volume=100
key_click=SOFT
startup_wav=off

[bluetooth]
keyboard_layout=JIS

[wifi]
wifi_enabled=off
wifi_ssid=
wifi_password=
wifi_auto_rtc=off
wifi_timezone_minutes=540
wifi_ntp_server=pool.ntp.org

[fkeys]
F1=DEMO.BAS,run
```

`cpu_mhz` と `wifi_enabled=on` が旧設定に残っていても、v0.91は安全のため起動時にCPU 150 MHz、Wi-Fi OFFとします。

## 25. Troubleshooting

### EditorでUndoできない

System InformationでPSRAMを確認します。`NOT AVAILABLE`なら編集自体は可能ですがUndo / Redoは使えません。PSRAM利用時は大規模INTERNAL ProgramStore等の他clientとのallocationも確認します。

### 長い行を入力できない

`BASIC>` promptは191文字です。2,047文字lineはFull-Screen Editor、SD／INTERNAL PSRAM ProgramStore、またはPCで編集して転送します。

### INTERNALが256行・191文字になっている

PSRAMが利用できずSRAM fallbackになっています。System InformationとPSRAM Diagnosticsで検出状態を確認します。PSRAM利用時のINTERNAL capacityは1,024行・2,047文字です。

### LISTが長くて止めたい

BREAK、Esc、Ctrl-Cのいずれかで停止できます。停止すると`[LIST BREAK]`を表示してpromptへ戻ります。

### SDでStorage Busyになる

音声、USB Storage、file transfer、診断など同時にSDを使う機能を停止します。Files previewとProgramStoreはlease競合を避ける設計です。

### MP3が再生できない

44.1 / 48 kHz、mono / stereoのMP3か確認します。USB Storageを停止し、SD mountとCPU切替errorを確認します。

### CPU profileを切り替えられない

USB Mass StorageまたはPSRAMの排他的診断処理を終了します。通常のWi-Fi / Bluetooth / LEDはprofile切替時に自動的に再初期化されます。

### Wi-FiまたはBluetoothが戻らない

profile切替後に数秒待ちます。再接続に失敗した場合は各メニューから再接続し、最後に再起動します。

### RTC N/A

source、I2C address、配線、電源を確認し、I2C SCANでdeviceを確認します。

### PDFと画面の表示が違う

PDF表紙がVersion 0.91であることを確認してください。旧版ではControl Center構成、Program Storage容量、Serial設定が異なります。
