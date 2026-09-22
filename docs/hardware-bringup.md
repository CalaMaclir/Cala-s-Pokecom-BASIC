# PicoCalc 導入・実機確認手順

対象：**Cala's Pokecom BASIC 0.6**、ClockworkPi PicoCalc、Raspberry Pi Pico 2 W（RP2350）

## UF2の導入

1. GitHub Actionsの成果物をダウンロードします。
2. Pico 2 WをBOOTSELモードにします。
3. `retrominibasic_picocalc.uf2`をRPシリーズのドライブへコピーします。
4. FAT32形式のSDカードを挿入して起動します。

## 基本確認

1. Version 0.6とBuild番号が表示されること。
2. 3行のステータス領域が同じテーマ色で表示されること。
3. 最下段にF1～F5の登録名が表示され、Shift中はF6～F10へ切り替わること。
4. 直接モードの`PRINT 2+3`が`5`になること。
5. HOME（Shift+Tab）でControl Centerが開くこと。
6. Control Centerから戻るとコンソールの配色と固定領域が復元されること。

## RUNとスクロール

次をそれぞれ確認します。

- 短いRUN
- 画面いっぱいまでPRINTするRUN
- 最下行ちょうどで終了するRUN
- LIST直後のRUN
- RUNの連続実行

いずれも「最終出力 → `[RUN] ...` → `BASIC>`」の順になり、古い文字列が同じ行に残らないことを確認します。

## SD

`DIR`、`LOAD`、`SAVE`、`SD STATUS`、`SD REMOUNT`を確認します。Alt+Sを複数回押し、`SCREENnnnn.BMP`が上書きされず連番で保存されることも確認します。

## Wi-Fi File Server

1. 起動時にWi-FiとFile ServerがOFFであることを確認します。
2. Wireless LANでWi-FiをONにして接続します。
3. Wi-Fi File ServerをSTARTし、表示URLをPCブラウザで開きます。
4. BASをUploadし、PicoCalcの`DIR`、`LOAD`、`LIST`、`RUN`で確認します。
5. BASとBMPをDownloadし、PC側でSHA-256を比較します。
6. 既存ファイルを上書きUploadし、転送途中に通信を切って旧ファイルが壊れないことを確認します。
7. Delete後、`DIR`から消えることを確認します。
8. 100 KB以上、可能なら1 MB程度のファイルでRAM使用量が増え続けないことを確認します。
9. STOP後にブラウザから接続できないことを確認します。
10. 再起動後にWi-Fi OFF、File Server OFFであることを確認します。

## XMODEM

PC → PicoCalc：

1. Tera TermでUSB COMポートを開きます。
2. `XRECV "TEST.BAS"`を実行します。
3. `File → Transfer → XMODEM → Send`でファイルを送ります。
4. 完了後、`DIR`、`LOAD`、`LIST`で確認します。

PicoCalc → PC：

1. `XSEND "TEST.BAS"`を実行します。
2. `File → Transfer → XMODEM → Receive`で保存先を指定します。
3. 転送後、PC側で元ファイルと比較します。

CRCエラー、キャンセル、タイムアウト後に通常の`BASIC>`へ戻ることも確認します。

## CPUとSTANDBY

FULL 150 MHz、NORMAL 100 MHz、ECO 75 MHzで、プロンプト、Fキー、SD、シリアル、Wi-Fiを確認します。再起動後はFULLへ戻ること、STANDBYでFile Serverが停止し、キー入力で復帰することを確認します。

## 回帰確認

- 文字列配列
- `ON GOTO`／`ON GOSUB`
- `PAINT`
- 3DHAT
- Alt+Sスクリーンショット
- RMBASIC.CFG／RMBASIC.BAK復旧
- Wi-Fi手動接続
- USB CDC／UARTコンソール
