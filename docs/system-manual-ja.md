# Cala's Pokecom BASIC System
## Version 0.94
## System Manual / システムマニュアル

本書は ClockworkPi PicoCalc 上の Cala's Pokecom BASIC（CPB）v0.94 の操作、設定、保存、通信、診断を説明します。BASIC 文法は `programming-reference-ja.md` を参照してください。

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

空の`BASIC>` promptでHOME（Shift+Tab）またはAlt+Cを押すとControl Centerを開きます。v0.93ではカテゴリ見出しと項目を同一画面に展開し、メニューlabelの`...`は使用しません。

```text
Files
Editor
    New Program
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
    System Information
Diagnostics
    Last Error / System
    PSRAM Diagnostics
Exit
```

表示はrow 37まで使用し、row 38を空行、row 39をFunction Key barとして確保します。項目が画面内に収まるため、通常は一覧全体を一度に確認できます。

- Up / Down: selectable itemを移動
- Shift+Up / Shift+Down: page移動が必要な一覧で使用
- Enter: open / execute
- Esc / HOME: back / exit

Network、Serial、Diagnosticsなどは同じ一覧のカテゴリ見出しと字下げで整理します。Serialの下のSerial Config／File Transferへカーソルを移動して開きます。

## 4. BASIC prompt

`BASIC>`ではDirect modeの実行、line number付きprogram編集、REPL commandを行います。interactive入力は従来どおり191文字境界です。2,047文字logical lineはFull-Screen Editorまたは外部file経由で扱います。

主なcommand:

- `LIST`: current programを表示。BREAK／Esc／Ctrl-Cで途中停止でき、`[LIST BREAK]`の後にpromptへ戻る
- `RUN`: current programを実行。処理開始時に`RUN...`を表示する
- `LOAD "name"`: 読み込み開始時に`LOADING... (filename)`、成功後に`LOADED filename`を表示する
- `EDIT`: Full-Screen BASIC Editor
- `NEW`: programとDirect stateをclear
- `CLEAR`: Direct scalarだけをclear

空のpromptではAlt+Eが`EDIT`、Alt+Rが`RUN`、Alt+Cが`MENU`と同じ動作です。入力途中や設定dialogでは誤作動せず、入力中文字列を保持します。serial consoleでは従来どおり文字commandを使用します。

長いprogramではLOADや初回RUNのcompileに時間がかかる場合があるため、v0.92では進捗表示を先に出して操作を受け付けたことが分かるようにしています。

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

一覧は属性欄5文字＋空白1文字＋名前です。通常ファイルは先頭6文字を空白にし、フォルダーと名前の位置を揃えます。サイズは右側に表示します。

```text
/sample/test
[DIR] games
[DIR] music
      mandel.bas
      julia.bas
```

未選択フォルダーは行末の余白まで専用配色、選択中は選択色を優先します。PROGRAMS／DIRECTORYとQuick Load／送信用pickerで同じ表示規則を使います。
SDルートからの表示パスは先頭 `/` 付き、rootは `/`。狭い欄はbasenameを優先してdirectory部分を `..` で省略し、削除・上書き確認では完全パスを複数行で表示します。表示整形は保存・実行・Quick Load割当を変更しません。最下部F-keyはbasenameのみです。


Files には2つのmodeがあります。

| Mode | 用途 | 主な操作 |
|---|---|---|
| PROGRAMS | BASIC program中心 | Enter LOAD、R RUN、E EDIT、N RENAME、Del DELETE、I INFO、F REFRESH |
| DIRECTORY | 現在フォルダーの全対象file | P PLAY/STOP、N RENAME、Del DELETE、I INFO、F REFRESH |

Left / Rightでmodeを切り替えます。両modeでフォルダーをたどれます。

| 操作 | 内容 |
|---|---|
| Enter（フォルダー） | 子フォルダーへ移動 |
| Esc / Backspace | 親へ移動。ルートでは終了 |
| Home | SDルートへ戻る |
| M | フォルダー作成 |
| N | ファイル／フォルダーの改名・移動 |
| Delete | 確認後に削除。空でないフォルダーは拒否 |
| I / F | 完全パス・種類・サイズ情報／再一覧化 |

一覧は128項目ごとのバッチで、上下端から次／前へ移動できます。並び替えはバッチ内です。操作前に音声プレビューを停止します。ロード前に未保存ソースのSave／Discard／Cancelを選びます。現在プログラムや親フォルダーの改名はsessionの保存先へ反映し、失敗時は元の名前への復旧を試みます。

### ファイル名とパス

- SDルート相対パスは区切りを含め最大79文字。表示用の先頭 `/` は内部名へ追加しません。
- 各成分はASCII英数字、`_`、`-`、`.`、内部空白を利用できます。
- 成分の先頭・末尾の空白／ドット、`..`、`.`成分、重複区切り、末尾区切り、バックスラッシュ、制御文字、非ASCII文字は禁止です。
- `/` はフォルダー区切りです。単独ファイル名には使用しません。
- program名は必要に応じて `.BAS` を補完します。

Filesの改名・作成dialogは現在フォルダー相対、先頭 `/` はルート相対です。LOAD／SAVE／DIR／FILESとSave Asはルート相対の完全パスを使います。例：`SAVE "GAMES/DEMO"`。保存先はFilesの表示フォルダーを移動しても変わりません。HTTPと受信転送はルート単一ファイル名の制限を維持します。

Rename と Delete は current program への影響を考慮して処理されます。Delete は確認画面を表示します。大文字・小文字だけを変える rename は FAT の条件により制限があります。

## 8. Full-Screen BASIC Editor

起動方法:

- `BASIC>` で `EDIT`
- `Control Center → Editor`
- Files PROGRAMS で対象を選び `E`

Editor は program 全体の複製を作らず、現在行用の約2 KiB buffer と ProgramStore を使用します。長い logical line は 320×320 LCD の幅に合わせて折り返されます。Classicは行番号欄12文字＋本文41文字、Structuredは論理行番号5桁＋スペース2個＋本文46文字です。論理行番号は`00001`から始まり、各logical lineの最初のvisual rowだけに表示します。折り返しの継続行は番号欄を空白にして、本文の開始位置を揃えます。この番号はEditor表示専用で、BAS保存やLISTには付きません。

### Editorキー

| キー | 機能 |
|---|---|
| Arrow keys | 文字・visual row単位の移動 |
| Shift+Up / Shift+Down | 1画面移動 |
| Home / End | logical lineの先頭 / 末尾 |
| Tab | 次の4文字区切りまでスペースを挿入。1回のTabは1回のUndoで戻せる |
| Alt+U / Bluetooth Shift+Tab | Structuredの現在行を最大4 space Outdent |
| Alt+M | Structuredの対応block境界へ移動 |
| Alt+R | working rowを反映してEditorを終了し、そのままRUN。BASへ自動SAVEしない |
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

Enterはカーソル位置でlogical lineを分割し、論理行頭では前に空行を挿入します。Classicでは新しい行番号を指定します。Structuredでは行番号の入力は不要です。F5でも新しい行を挿入できます。Classicの番号は手動で指定し、自動再採番しません。先頭行が10なら、論理行頭でEnterを押して5の空行を前へ挿入できます。前行があれば「前行番号 < 新番号 < 現在番号」、途中分割では「現在番号 < 新番号 < 次行番号」を満たす必要があります。

論理行頭のBackspaceは前行へ、論理行末のDeleteは次行と連結します。Classicは前の番号を残し、GOTO等の参照先を自動変更しません。コロン・空白は自動追加しません。行長上限超過、不正番号、空き番号なし、キャンセルでは元の行を保持します。分割・連結は各1回のUndo／Redoで往復できます。

```text
00001  REM
00002  REM
```

Structuredの表示番号は論理行の先頭だけに付き、折り返し行の7文字欄は空白です。空行にも番号が付き、保存ソースへ表示番号を追加しません。

Alt+Mは`IF`／`END IF`、`FOR`／`NEXT`、`WHILE`／`WEND`、`DO`／`LOOP`、`SELECT CASE`／`END SELECT`、`FUNCTION`／`END FUNCTION`の対応先へ移動します。`ELSE`／`ELSEIF`／`CASE`／`CASE ELSE`では、それぞれを開始した`IF`／`SELECT CASE`へ移動します。文字列やcomment内のkeywordは対象外です。Alt+Uは本体キーボードで確実に使えるOutdentです。本体Shift+TabはKeyboard BIOSによってHOMEと区別できない場合があります。

v0.94ではCompile ErrorとRuntime Errorの両方を記録します。失敗後に`EDIT`またはControl CenterのEditorを開くと、変更していない該当Structured row／Classic行番号へ移動します。下部に`COMPILE ERROR AT ROW/LINE`または`RUNTIME ERROR AT ROW/LINE`を表示します。ソースの変更・LOAD・NEW・モード／filename変更で古い位置を無効にします。Direct実行、行情報やsourceなし、保存元ファイル削除済みの場合は通常位置で開きます。Last Errorは成功やBREAKでも記録を保持し、次の失敗で置換します。再起動すると消えます。

Structuredの行末でEnterを押すと既存の先頭空白量を次の行へ引き継ぎ、IF／FOR／WHILE／DO／SELECT CASE／FUNCTIONの開始は4 spacesを加えます。新しく入力したEND IF／NEXT／WEND／LOOP／END SELECT／END FUNCTIONやELSE／ELSEIF／CASE／CASE ELSEは、Enter時に対応する開始行へ揃えます。CASEはSELECTの1段内側、bodyはもう1段内側です。Enter＋整列＋次行indentは1回のUndoで戻せます。

開くだけ、カーソル移動だけでは書き換えません。既存の閉じる行が未編集ならその空白も維持します。途中位置のEnterは元のtextをそのまま分割します。Classicでは従来処理です。文字列やREM／apostrophe commentはblockと誤認しません。構造が不正／対応開始行が不明なら無理に揃えません。既存Tab／Alt+U操作と4-space方針を維持します。Editorは将来用にSUB／END SUBのindentも認識しますが、実行言語のSUB対応を追加したわけではありません。

### Undo / Redo

PicoCalc PSRAM が利用できる場合、Editor は256 KiBを履歴用に取得し、おおむね31件の coalesced Undo と31件の Redo を保持します。連続入力はまとめられる場合があります。Editor Historyはエディタを閉じると破棄されます。program本体の保存先はProgram Storage backendに従い、INTERNAL PSRAM modeではprogram source自体もPSRAM ProgramStoreに保持されます。

PSRAMがない場合も編集と保存は利用できます。F4 / F9 は無効で、状態行に理由が表示されます。

### 性能

v0.93 はカーソルだけの移動を部分再描画し、key repeat中の全画面描画を抑えます。SD viewport は1回の lease / open / close でまとめて読み出します。性能測定用 UF2 は通常利用向けではありません。

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

v0.93 のAudioは3層です。

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

2.4 GHz WLANへ接続します。Wi-Fi master stateはsession-onlyで、登録profileがあっても起動時は必ずOFFです。

`Known Networks`では最大5件のSSIDを管理できます。各profileは個別にEnable / Disable、Delete、Connect Nowを実行できます。独立したChange Password項目はありません。同じSSIDを`Scan & Add Network`でもう一度選ぶと、IP取得まで接続できた後にpasswordを更新し、Enable／Disable状態は維持します。Disabled profileは情報を保持しますが、Known Networksの手動接続と自動接続の候補にはなりません。ただしScan & Addで明示的に選んだSSIDへの接続・更新は実施し、Disabledの設定を維持したまま接続します。接続中profileをDisableまたはDeleteすると切断しますが、別profileへ勝手に接続しません。

`Scan & Add Network`は周辺APを5秒間観測してRSSI順で表示し、選択したSSIDを空きslotへ登録します。一覧の`K`は登録済みSSID（Disabledも含む）、`*`はsecured APを示します。secured APのpassword入力文字は小型画面で確認できるよう、そのまま表示されます。入力中は周囲から見られない場所で操作してください。保存後のpasswordはKnown Networks、System Information、debug log、serial statusには表示されません。同じSSIDを再登録した場合は新しいslotを消費せずpasswordを更新し、Enabled状態を維持します。5件使用中の新規SSIDは拒否されるため、先に`Known Networks`で不要なprofileを削除します。

`Scan & Add Network`は、secured APではpassword入力後、open APでは入力なしで、そのSSIDへの接続とDHCPによるIP取得まで実行します。IP取得成功後にprofileを保存し、`CONNECTED - PROFILE SAVED`を表示して接続を維持します。password認証だけで切断するtestは行いません。

password誤りや接続timeoutの場合は、同じSSIDのまま`Retry Password`で再入力できます。DHCP／IP取得や接続開始の失敗は`Retry`で同じpasswordのまま再試行できます。`Cancel`またはEsc／BREAKで終了すると、新規profileは作成せず、既存profileとpasswordも変更しません。IP取得待ちの途中でも中断可能です。接続失敗・中断後やWi-FiをOFF／ONした際は、残る切断処理の完了を待ってから次の接続へ進みます。

Wi-FiをONにするか`Auto Connect Now`を選ぶと、5秒間の観測結果からstrongest Enabled AP、つまりEnabled profileをRSSIの強い順（同値なら小さいslot順）に試します。5秒の観測後、無線側のscanが続いている場合は`FINISHING WIFI SCAN...`を表示し、終了してから接続を開始します。終了待ちが10秒を超えると`WIFI SCAN FINISH TIMEOUT`を表示します。接続・認証・DHCPが失敗すると次候補へfallbackし、全候補の失敗時は最後の接続errorを表示します。Scanとその終了待ち、接続・認証、DHCP待ち、NTP同期はEscまたはBREAKで中断できます。Scan driverを物理的に即時停止できない場合も画面操作へ直ちに戻り、driver内のscanが終了するまでは再scanを`SCAN BUSY - TRY AGAIN`として拒否します。Wi-Fi masterは起動ごとにOFFへ戻り、profileだけが保存されます。

接続後はNTP、Wi-Fi File Serverを利用できます。接続できない場合はSSID、password、2.4 GHz、電波状態を確認します。

`Set NTP Server`で時刻serverを変更できます。`Auto Time: ON`による接続後の自動同期、または`Sync Time Now`が失敗すると、`NTP SYNC FAILED`画面に失敗理由と設定serverを表示します。Wi-Fiが接続中なら`WI-FI STILL CONNECTED`を表示し、接続は継続します。任意のkeyで元menuへ戻れます。NTP失敗では時刻を更新しません。同期中のEsc／BREAKは中断として扱い、失敗画面は表示しません。

## 15. Bluetooth Keyboard

Bluetooth Classic HID / BLE HID Keyboard に対応します。Pair、Reconnect、Disconnect、Forget、JIS / US layoutを選択できます。Paired DevicesにはKeyboardとして登録された機器だけを表示します。

v0.92のBluetoothはkeyboard入力用途です。旧版のClassic SPP file transferとは役割が異なります。

## 16. Wi-Fi File Server

Wi-Fi接続後、HTTP File Serverを開始すると同一LANのbrowserからSD rootへupload、download、deleteできます。画面に表示されるURLへ接続してください。

利用後はserverを停止します。公開ネットワークでは使用しないでください。

## 17. File Transfer

File TransferのtransportはAUTO、USB CDC、UART0です。Bluetooth SPP/RFCOMMはv0.89以降の現行構成には含まれません。

| Protocol | Send | Receive |
|---|---|---|
| XMODEM | single-file | single-file |
| YMODEM | single-file | single / multi-file batch |

v0.92 Stage 1ではtransfer hot pathを次のように最適化しています。

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

v0.92 はprofile切替時に必要ならCYW43関連のWi-Fi / Bluetooth / Board LED serviceを停止・再初期化します。利用者が先に手動停止する必要はありません。USB Mass Storage中、PSRAM診断や競合処理中は安全のため切替を拒否します。

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

v0.93のPSRAMは単一ownerではなくmulti-client allocatorで管理されます。INTERNAL ProgramStoreは通常約2 MiBをclaimし、Editor History、SD Cache、DirectState、Compiled Cacheと同時利用できます。VMのhot opcode実行は内部SRAMで行います。System InformationにはKeyboard I2Cの状態、error／consecutive／recovery count、startup phase、BIOS報告、SDA／SCL状態も表示されます。本体キーボード障害の記録にはこれらとuptime、USB給電状態を使用してください。

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
wifi_auto_rtc=off
wifi_timezone_minutes=540
wifi_ntp_server=pool.ntp.org
wifi_profile1_enabled=on
wifi_profile1_ssid=HOME
wifi_profile1_password=xxxxxxxx
wifi_profile2_enabled=off
wifi_profile2_ssid=MOBILE
wifi_profile2_password=yyyyyyyy
# profile3～profile5も同形式

[fkeys]
F1=DEMO.BAS,run
```

`cpu_mhz` と `wifi_enabled=on` が旧設定に残っていても、安全のため起動時にCPU 150 MHz、Wi-Fi OFFとします。旧形式の`wifi_ssid` / `wifi_password`だけが存在する設定は、memory上でProfile 1（Enabled）へ移行し、次回の通常保存で新形式になります。新profile keyが1件でも存在する場合は新形式を優先します。保存済みpasswordはKnown Networks、System Information、debug log、serialへ表示しません。Scan & Addで入力している間だけ文字がvisibleです。

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

PDF表紙がVersion 0.94であることを確認してください。旧版ではControl Center構成、Program Storage容量、Editor shortcut、Wi-Fi profile仕様が異なります。

## Structured programの作成・編集（v0.93）

Control CenterのEditor > New ProgramからClassic BASIC／Structured BASICを選びます。未保存sourceがあればSave／Discard／Cancelを選択します。Structured新規作成後はEditorが開きます。EDITは現在sourceをそのmodeで開きます。promptのNEWはClassicを作成します。

Editor上部に[CLASSIC]／[STRUCTURED]を表示します。StructuredのROWはsource位置です。番号付き入力dialogを表示せず、Enterはカーソルで分割、論理行頭Enterは前へ空行挿入、行頭Backspace／論理行末Deleteは前／次行と連結します。colonは自動挿入しません。v0.94では行末Enterに限り自動indentを補助します（上記参照）。F5は空行挿入、F4/F9はUndo/Redo、F7はrow移動です。保存／再LOADで空行とindentを保持します。Tabはカーソルのlogical columnを基準に次の4文字区切りまでスペースを挿入します。Tab文字自体は保存しません。行長上限に収まらない場合は一部だけ挿入せず、元の行を保持します。

Structuredがactiveなとき、promptの10 PRINTなどの番号付き入力はSTRUCTURED PROGRAM - USE EDITORで拒否し、sourceを変更しません。Direct statementとREPL commandは従来どおり利用できます。LISTとBAS保存に仮想番号は付きません。外部fileもLOAD可能ですが、Classic／Structuredの混在は拒否します。空／空行だけのfileは現在sessionのmodeを保持します。

Program Storage容量は従来と同じです。PSRAM INTERNAL／SDは1024行×2047文字、SRAM fallbackは256行×191文字です。session recovery、USB Storageからの復帰、file／parent-directory rename、Storage切替でもmodeを維持します。

長いpathは表示幅に応じbasenameと末尾directoryを優先して..で省略します。表示だけの短縮であり、実際のpathとQuick Load assignmentは保持します。最下部F-keyは従来どおりbasenameだけを表示します。

配布ZIPのexamples/structuredに機能確認programがあります。square=144、wrap=[CPB]、local_scope=16/100、global_scope=10/30/30、factorial=120、block_if=GOODを確認してください。depth_limitは深さ超過error、break_cleanupはEscで停止後に別programを正常RUNできることを確認します。正常例・意図的エラー例・中断例の操作はexamples/README.mdを参照してください。

session metadataが破損し復元候補を検証できない場合、作業sourceをRECOVER0000.BASなどへ退避して空sessionを開始します。Filesから確認／LOADしてください。退避できなければsourceを削除せずerrorを返します。旧version 1 sessionはClassicとして読み込み、次の保存でversion 2へ移行します。


## 26. STANDBYと更新時のsession

STANDBY、POWER／Alt+P、System → Power / CPU → STANDBY NOWでRAMを保持した待機に入ります。本体キーで復帰します。USB Storage所有中は先にPCで取り外し、CPBへ返してください。LCDとキーボードのバックライトを消し、LCDもDisplay OFF／Sleep INへ移行し、復帰時に内容と明るさを戻します。

Keyboard BIOS 1.7・build696でLCD消灯、キー復帰、10回連続の待機／復帰を確認しています。他のBIOSで同じ結果を保証するものではなく、CPB導入の一律必須条件としてBIOS更新を要求しません。STANDBYは完全電源断ではなく、消費電力の定量保証はしていません。

session形式はversion 2です。旧version 1をClassicとして読込み、次回保存で移行します。Compiled Cacheはformat 6で、session形式とは別物です。旧形式・CRC不一致・ソース変更はcache missとなり再compileします。更新前は必要なBASと設定をSD／PCへ保存し、復元候補の内容を確認してから採用してください。USB Storageからの復帰は再indexとcache失効を行い、source modeを維持します。


## v0.94 Stage 4-6: Diagnostics / Last Error

Control CenterのDiagnostics > Last Error / System、またはREPLのDIAGNOSTICSで開きます。System Informationと同じcollector／snapshot／Serial rendererを使い、Last Errorを含むpageから表示します。System Informationには先行Stage 1+2の情報を保持します。左右でpage移動、Rでsnapshot refresh、SでUSB CDC送信、EscまたはEnterで戻ります。大量の行は53 columnsにwrapして27行ずつ表示します。

Last Error: None、またはtype、message、program、source mode、Classic line／Structured row、FUNCTION、call depth、最大4件の内側からのFUNCTION trace、IL PC、error時uptimeを表示します。traceが深い場合は省略を明示します。error発生後にsnapshotを取得すれば、その記録をそのままSerialへ出せます。

Audio powerはOff／On／Idle grace (10 sec)／Ramping up／Ramping downを現在状態から表示します。既存の10秒delayとPWM rampの動作は変更しません。Keyboardはcached status／last I2C error／recovery countを表示し、diagnosticsのためのBIOS問い合わせ、scan、I2C resetは行いません。SD再mount、Wi-Fi変更、Audio停止／開始、Serial設定変更、CPU Clock変更も行いません。

USB CDCがhostにopenされていない場合やX/YMODEM等がSerialを所有中の場合は送信を拒否します。snapshotをrefreshしない限り表示と送信は同じ取得内容です。途中切断はreport不完全の案内を表示します。Wi-Fi password／token／API keyをcollectorへ入れません。BASIC sourceのliteralに認証情報がある可能性を考慮し、source本文を共有用Serial reportへ含めません。source previewはエラー発生時のconsoleに表示します。

heap実測値、詳細Reset Reason／Watchdog状態は今回追加していません。信頼できない値は推定せず、既存のFree RAM / HeapはN/Aを維持します。Last ErrorはBASICのcompile／runtimeを対象とし、REPLのLOAD失敗、Audio、Keyboard等の個別errorは既存の専用表示を使います。

## v0.94: 未保存Programの保護

EditorとREPL上部のprogram名に付く `*` は未保存変更を表します。カーソル移動、RUN、runtime errorだけでは未保存になりません。同じClassic lineの再入力や存在しないlineの削除も変更として扱いません。

NEW、LOAD、F1-F10 quick LOAD/RUN、Filesから別programを開く操作、Control Center / Program StorageのNew Programは、未保存のprogramがあれば次の確認を表示します。

| キー | 動作 |
|---|---|
| S: Save | 現在のprogramを保存してから操作します。未命名ならSave Asで名前を入力します |
| D: Discard | 確認した操作の成功時に現在の変更を破棄します。LOAD失敗時は元programと未保存状態を維持します |
| ESC: Cancel | 操作を中止し、元のprogram / Files画面へ戻ります |

保存失敗時はprogramと未保存状態を維持し、errorを表示します。通常のSAVE、EditorのF1 Save、Save Asで再保存できます。Save As先に既存programがあれば上書き確認を行います。

EditorのESC終了はsourceを現在のProgramStoreに保持します。既存のSave / Keep Unsaved / Cancelを使用し、Keep Unsavedを選んでもsourceを捨てません。Editor内で保存した状態へUndoしたときは未保存表示が消え、Redoすると再び表示します。未保存の状態でEditorを開き直した場合は、入室時より前の保存内容までは追跡しません。

SD backendの既存editing session（RMBASIC.SESと専用work file）を復旧に使用します。次回起動時に有効なsessionを復旧して案内し、未保存の復旧sourceがあればAUTORUNより優先します。これは通常のBAS保存とは別です。新たな周期的Autosaveは追加しません。INTERNAL SRAM / PSRAMのsourceは電源断後の復旧対象ではないので、必要なprogramはSAVEしてください。

REPLの `INFO` は現在の状態を短く表示します。`LASTERROR` は直近のCompile / Runtime / Direct errorを再表示します。詳細はDIAGNOSTICSまたはSystem Informationを開き、SでUSB Serial Reportを出力できます。両commandともBASIC program内のstatementとしては使用しません。

`DATE$()` / `TIME$()` と新しい文字列関数の詳しい構文・errorはProgramming Reference Manualのv0.94 Stage 8 / 9追加項目を参照してください。
