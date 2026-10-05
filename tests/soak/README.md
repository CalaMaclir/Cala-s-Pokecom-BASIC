# v0.94 最終実機Soak / Smoke

対象は最終CIの通常版 `build/CPokecombasic.uf2`。Stage 1～9の実機承認と今回の最終試験は分けて記録します。試験用BASは通常配布ZIPに含めません。

## 準備と記録

SDの既存BAS／設定を保存し、Filesで新しい空directory `CPB094TEST` を作ります。このdirectoryと2つのBMP名は試験専用です。既存の同名画像がある場合は別名へ変更してから実行してください。SDの物理抜去・破壊的試験は行いません。

起動直後、`INFO`、`LASTERROR`、Diagnostics → S Serialから次を記録します。

| 時点 | Free RAM / Heap / PSRAM | clock / uptime | SD / owner | Wi-Fi | Audio / power | serial | keyboard error / nack | Last Error |
|---|---|---|---|---|---|---|---|---|
| 起動直後 | | | | | | | | |
| warm-up後 | | | | | | | | |
| 50回後 | | | | | | | | |
| 100回後 | | | | | | | | |
| 150回後 | | | | | | | | |
| 200回後 | | | | | | | | |
| 60分後 | | | | | | | | |

実装が取得できないHeap等はN/Aのまま記録します。単発の揺らぎと継続減少を分け、warm-up以降の傾向を比較します。負荷を止めてIdleへ戻した同じ条件で前後を比較してください。

## 画像と長時間試験

`image-classic.bas` と `image-structured.bas` は各RUNで50回、SAVEIMAGE／SAVE IMAGE両方を保存・CLS・LOADIMAGEし、赤い画素のPOINT値が一致することを確認します。各modeで4回RUNし、各batch後にINFOとSerialを採取します。BMPファイルは2個を上書きし、数百個の試験画像は作りません。50回だけで電気的安定性を判定せず、以下の操作を組み合わせて60分以上試験します。

- Files: directory移動、音声P再生／P停止／ESC停止、画像選択、戻る、再入場。ESC停止時に勝手に親directoryへ移動しないこと。停止キーのrepeat後の独立したESCで通常の戻る操作ができること。
- Audio: WAV／MP3／MML、Key Click OFF／SOFT／CLASSIC／SHARP。停止→Idle→10秒のgraceとbias ramp後の省電力、二重音・プチ音なしを実測・聴取。起動付近の1.0 Wへ戻る既存確認も再確認。
- Editor: Enterのindent／closer整列、Undo／Redo、F1 Save、F6 Save As、Exit→再入場。PSRAMが利用可能な構成でUndo／Redoを確認。
- Program Safety: LOAD→編集→NEW／LOAD／Files／quick keyでSave／Discard／Cancel。Cancelは全内容／filename／mode／dirty維持。安全な存在しないdirectoryへのSaveで失敗→dirty維持→再保存成功。
- Error: `A=1/0`、存在しないBMP読込、正常PRINT／RUN、LASTERROR、Editorへ復帰を繰り返す。Last Errorは最新失敗だけを保持し、成功後も残る。
- Diagnostics: Audio中／停止後、Wi-Fi connect／disconnect／reconnect／NTP、Files／Editor／Error後。INFOとsnapshotが整合し、表示で状態変更なし。S Serialを繰り返し、転送中は安全にbusy扱い、終了後REPLとYMODEMを確認。
- Wi-Fi: 最大5 profile、enable／disable／delete／scan／cancel、password入力表示仕様。実際のAPと照合。
- SD / USB: read／write／directory、USB Storage→安全なreturn、USB接続有無の比較、CDC REPL／XMODEM／YMODEM高速転送。復旧済みの問題を再現しなければ新たな不具合にしない。
- Keyboard: キー抜け／重複／stuck／I2C error／write nackを前後の受動diagnosticsで比較。独立recovery firmwareは使わず通常UF2を使用。

## 最終Smoke

Boot → REPL → INFO → Editor編集 → SAVE → RUN → Files音声P／ESC → 両画像構文 → Runtime Error → LASTERROR → EDIT → Diagnostics → Serial report。最終UF2のversion／build／SHAがmanifestと一致することも確認します。

結果は `docs/release/v0.94-release-checklist.md` の実機recordへ転記します。実機未実施をPASSにしません。合格した最終SHAを確定後、正式リリース手順に従ってmain統合・公開用同期・tag／Releaseへ進みます。
