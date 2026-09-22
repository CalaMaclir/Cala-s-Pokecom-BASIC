# Cala's Pokecom BASIC アーキテクチャ

## 現行バージョン

製品版数は**0.6**です。GitHub Actionsの実行番号はBuild番号として別に組み込まれます。

## 系譜

**Cala's Pokecom BASIC**は、Cala Maclirが制作した**RetroMiniBASIC**の言語処理系、コンパイラー／中間言語、仮想マシンを基盤に、Cala Maclir本人がPicoCalc向けに設計・実装した環境です。

## レイヤー

### BASICコア

ハードウェアから独立したソース管理、コンパイラー、RetroMini中間言語、VM、実行状態、直接モードを担当します。Pico SDKの表示・入力・SD・Wi-Fi APIは直接呼びません。

### REPL／デバイスシェル

行編集、`LIST`／`RUN`／`LOAD`／`SAVE`、直接モード、3行ステータス、F1～F10、HOME Control Center、設定、日時、コンソール経路、XMODEMを担当します。

### プラットフォーム層

320×320 LCD、PicoCalcキーボード、バッテリー、Caps Lock、バックライト、時計、任意のPCF8563、USB CDC／UART、グラフィック、画面キャプチャを抽象化します。

### ストレージ層

SDカード上のBASICファイル、`RMBASIC.CFG`／`RMBASIC.BAK`、BMP、`AUTORUN.BAS`を扱います。XMODEM受信とHTTP Uploadは共通の安全な一時ファイル置換を利用します。

FatFSは同時アクセスさせません。HTTP転送などがSDを使用中はstorage busy状態とし、LOAD／SAVE／DIR／スクリーンショット／設定保存／XMODEMとの競合を防ぎます。

### ネットワーク層

Pico 2 WのCYW43とlwIPを使用し、Wi-Fiの有効化、SSID接続、DHCP、DNS、NTP、HTTP File Serverを担当します。File ServerはlwIP raw APIとメインループ上の処理を連携させ、callbackから長時間のFatFS処理を行わない構成です。

HTTP File Serverは次の制約で小さく保ちます。

- ポート80、Connection: close
- 原則1クライアント／1操作
- GET／PUT／DELETEのみ
- SDルートの単一ファイル名だけを許可
- `..`、`/`、`\\`、制御文字を拒否
- Content-Length必須、chunked非対応
- 小さな固定バッファによるストリーミング
- 起動ごとのセッショントークン

## 表示モデル

通常コンソールでは上3行をステータス、下1行をFキー表示として固定します。スクロール領域はその間だけです。グラフィックは物理320×320全体を使用でき、REPL復帰時に固定領域を再描画します。

## 起動・停止ポリシー

- 起動時CPU：FULL 150 MHz
- 起動時Wi-Fi：OFF
- 起動時File Server：OFF
- Wi-Fi OFF、切断、STANDBYでFile Serverを停止
- CPUプロファイルは保存せず、再起動時にFULLへ復帰

## 移植方針

BASICコアとユーザー向けの言語仕様は可搬に保ち、表示、入力、ストレージ、時刻、ネットワーク、電源管理をプラットフォームサービスとして分離します。

リポジトリ名、ビルドターゲット、設定ファイル名には互換性のため`RetroMiniBASIC`／`RMBASIC`が残っていますが、ユーザー向け製品名は**Cala's Pokecom BASIC**です。
