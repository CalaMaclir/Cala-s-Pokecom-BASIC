# Stage 5 performance fixtures

通常版と整数FOR実験版で、この同じファイルを実行してください。
150 MHz、同じSD／Program Storage、PROFILE OFFに固定します。
初回RUNを1回、その後sourceを編集せずcache-hit RUNを5回以上記録し、中央値を比較します。
PROFILE COUNTは別RUNで取得してください。
`[RUN]`はcompile/cache restore後のVM execution時間です。

| File | Expected output |
|---|---:|
| for-empty.bas | 1000001 |
| for-sum.bas | 1.25025e+07（数値12502500） |
| for-nested.bas | 1000000 |
| for-step2.bas | 1000002 |
| for-negative.bas | -1 |
| for-function.bas | 1.25025e+07（数値12502500） |
| data-select-exit.bas | 6000CPB |

比較A/Bは受入済みStage 4 build808→Stage 5通常版です。
比較B/Cは同じStage 5 headの通常版→`-intfor-exp`です。
実験版はEXPERIMENTAL／NOT FOR RELEASE。変数・式はfloat32のままです。

| File / firmware | First RUN | Hit 1 | Hit 2 | Hit 3 | Hit 4 | Hit 5 | Hit median |
|---|---:|---:|---:|---:|---:|---:|---:|
| （実機測定値を記録） | | | | | | | |

Stage 3/4の動作確認は`../hardware-regression.bas`で`STAGE3 PASS`を確認してください。
詳細は`docs/development/archive/v0.93/v0.93-stage5ab-performance-integer-research.md`を参照してください。
