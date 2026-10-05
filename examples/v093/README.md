# v0.93 開発版の実機サンプル

ZIP内のBASをSDへ転送してLOAD、RUNします。番号なしはStructured、番号付きはClassicです。各sampleを2回RUNし、初回とcache-hit時の結果が一致することも確認してください。

| File | 確認内容 | 期待出力 |
|---|---|---|
| control-data-select.bas | DATA + SELECT + EXIT DO + RESTORE | ONE、TWO OR THREE×2、FOUR PLUS×2、SUM=15、RESTORE=1 |
| exit-cleanup.bas | EXIT DOがnested FORを解放 | EXIT AT=3、その後1、2、3 |
| data-classic.bas | Classic、配列READ、string、RESTORE | 3、CPB\|A,B、1 |
| function-read-select-exit.bas | FUNCTION local READ + SELECT + EXIT FOR | 4、5、6 |
| select-string-once.bas | computed string selectorと1回評価 | MATCH、CALLS=1 |
| math-intops-classic.bas / math-intops-structured.bas | Stage 3A/3B回帰 | LOG/LN、ASIN/ACOS/ATAN2、整数除算・shift・XOR |

Structured EditorでAlt+MのSELECT↔END SELECT、CASE／CASE ELSE→SELECTとnested SELECTも確認します。詳細チェックリストは`docs/system-manual-ja.md`を参照してください。
