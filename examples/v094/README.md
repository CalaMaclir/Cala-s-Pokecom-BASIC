# v0.94 実機確認

## Stage 7-9

stage8-strings-classic.bas / stage8-strings-structured.basの期待出力:

    [CPB]
    bXXa
    A   B
    8!!!

stage8-datetime.basはDATE / TIMEまたはNTPで時計を設定してから実行し、YYYY-MM-DD / HH:MM:SSの表示を確認します。

未保存ProgramのS / D / ESC、保存失敗、Undo / Redo、INFO / LASTERRORの手順は`docs/system-manual-ja.md`。

## Error → Editor → Diagnostics

`error-classic.bas`と`error-function.bas`をSDへコピーします。

1. `LOAD error-classic`、`RUN`で意図的にzero divisionを発生させます。Classic Line 120と`A=1/0`が表示されます。
2. `EDIT`でline 120へ移動します。直してRUNできることを確認します。
3. `LOAD error-function`、`RUN`ではFUNCTION Divide、source row 6、`RETURN A/B`、呼出元row 3、depth 2が表示されます。
4. `EDIT`でrow 6へ移動します。sourceを変更すると古いerror位置へのjumpは無効になります。
5. `DIAGNOSTICS`またはControl Center > Diagnostics > Last Error / Systemを開きます。左右page、R refresh、Esc returnです。
6. PC側でmicro-USB CDCのterminalをopenし、Sでreportを送信します。type／message／line or row／FUNCTION／trace／uptimeを確認します。source本文とpasswordは共有reportへ入りません。

保存元fileを削除した場合、Direct入力のerror、NEW／別file LOAD／mode変更後はEditorが通常位置で開くことも確認します。Last Errorは正常実行とBREAKでも保持し、次の失敗で更新します。再起動でNoneに戻ります。

## Structured自動indent

Control Center > Editor > New Program > StructuredでIF、FOR、WHILE、DO、SELECT CASE、FUNCTIONの行末Enterを確認します。4 spacesのindent、typed close／ELSE／ELSEIF／CASEのEnter整列、F4 UndoとF9 Redoを確認します。未編集の既存fileは自動整形しません。文字列`PRINT "END IF"`、`REM END IF`、`' END IF`も試します。SUBは実行言語では未対応です。

## 画像と復帰

既存のimage-roundtrip-classic.bas／image-roundtrip-structured.basでSAVEIMAGE→CLS→LOADIMAGE、旧SAVE IMAGEを確認します。V094SC.BMP／V094OLD.BMP／V094PART.BMPを上書きします。

`LOADIMAGE "NONE.BMP",0,0`でmissing fileを確認します。壊れたBMPはPCで`BROKEN.BMP`に2文字の`BM`だけを書いてSDへコピーし、`LOADIMAGE "BROKEN.BMP",0,0`を実行します（既存fileを上書きしないでください）。どちらも安全にerrorへ戻り、続けてEDIT／FILES／MENU／DIAGNOSTICS／Serial／BEEPを使えることを確認します。

FilesのESC／Pの一度だけの処理、WAV／MP3の再生状態、キークリック、省電力復帰時のpopなしも再確認してください。
