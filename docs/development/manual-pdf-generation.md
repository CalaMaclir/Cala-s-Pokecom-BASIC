# Japanese Manual PDF Generation Procedure

現行版と不変のv0.92マニュアルの生成・再現・QA手順。対応modeとバッジは[Programming Reference](../programming-reference-ja.md)を参照します。

## 正本・入力・出力

Markdownの正本は`docs/install-manual-ja.md`、`docs/system-manual-ja.md`、`docs/programming-reference-ja.md`。正式な3冊の生成器は`tools/generate_manual_pdfs.py`。`tools/build_manual_pdf.py`は過去の単冊生成経路で、現行用の別generatorを作らない。

現行はv0.94。出力は`docs/Cala-Pokecom-BASIC-v<version>-{Install-Manual,System-Manual,Programming-Reference}-ja.pdf`と`docs/manuals-manifest.json`。**3冊のcurrent PDFとmanifestを原稿と同じ開発repositoryへ保存する**。Actions artifactのみで代用しない。README日本語/英語と`docs/manual-ja.md`は現行PDFに直接linkする。

Version更新時はgeneratorのversion選択・source routing・validator・workflow・リンクを同時更新する。現在CLIは0.94と旧0.92を扱う。

## Pinned environment / font

Python 3.12、`tools/requirements-manuals.txt`のReportLab 4.4.9 / japanize-matplotlib 1.1.3を使用する。package内IPAex GothicのsubsetをPDFへ埋め込み、ASCII codeはCourier。font SHA256をmanifestへ記録する。CID fallbackは日本語欠落の再発を避けるため正式生成で使わず、独立font fileを無断で同梱しない。licenseは`docs/IPA-Font-License.txt` / THIRD_PARTY_NOTICESを保持する。

`--font` / `CPB_JAPANESE_FONT`は調査用に利用できるが、正式生成はpinned fontを使用する。fontやPython/ReportLabを変えた場合、同一bytesとみなさず再QAする。

## 生成・検証

完全な開発source treeのrootで実行する。配布artifact ZIPは完全source treeではない。

```sh
python3 -m pip install -r tools/requirements-manuals.txt
python3 tools/publish_release.py source-audit
python3 tools/validate_manual_reference.py
python3 tests/manual_reference_layout_test.py
python3 tools/generate_manual_pdfs.py --version 0.94
python3 tools/validate_release_docs.py
```

原稿・generator・画像を変更したPRではPDFとmanifestを同時commitする。workflowの再生成後に差分がないことを確認する。

## Reproducibility / manifest

invariant Canvasにより生成日時由来の変動を抑える。manifestはVersion、source/PDF/generator SHA256、ReportLab、font/package/hash、canvas invariantを記録する。

同一pinned environmentで2回生成し、**実際のPDF SHA256**を比較する。異なるPython / ReportLab / font環境間のbyte一致は保証しない。localとActionsのmanifestが異なればsource・generator・fontを比較し、差分PDFを再QAする。

```sh
python3 tools/generate_manual_pdfs.py --version 0.94 --output-dir build/manuals-repro
cmp docs/Cala-Pokecom-BASIC-v0.94-Install-Manual-ja.pdf build/manuals-repro/Cala-Pokecom-BASIC-v0.94-Install-Manual-ja.pdf
cmp docs/Cala-Pokecom-BASIC-v0.94-System-Manual-ja.pdf build/manuals-repro/Cala-Pokecom-BASIC-v0.94-System-Manual-ja.pdf
cmp docs/Cala-Pokecom-BASIC-v0.94-Programming-Reference-ja.pdf build/manuals-repro/Cala-Pokecom-BASIC-v0.94-Programming-Reference-ja.pdf
```

## Reference badge rendering

Markdownの`### Command Name`と直後のmode metadataを、現行PDFで1つの分割されないheadingへ変換する。Command Name・`Classic 値`・`Structured 値`を同じbaselineに置き、独立したオレンジ枠を2つ描画する。長いCommand Nameは幅を検査し、見出し文字を必要な範囲だけ縮小する。収まらない場合は生成を失敗させ、黙って2行へ戻さない。値は対応 / 非対応 / 共通 / 利用可。

133項目の現行inventoryをreference validatorで照合し、metadataの重複・見出しからの分離も拒否する。rendering regressionはClassicのみ、Structuredのみ、両対応、REPL共通と最長見出しを確認する。正式QAでは4種類を実際のPDFで目視し、見出し・バッジ・本文のpage splitも確認する。

## Visual QA

```sh
pdftoppm -png -r 110 docs/Cala-Pokecom-BASIC-v0.94-Programming-Reference-ja.pdf build/reference-qa
```

3冊の全pageを確認する。日本語glyph、cover / TOC / bookmark / page number、header/footer、table、code spaces/indent、wrap、page break、1行バッジ、clip/overlapを点検する。text extractionだけではlayout PASSとしない。page数・hash・確認範囲をPRまたはQA記録に残す。PDF QAから実機・Windows updater成功を推測しない。

## Historical v0.92 reproduction

`docs/archive/v0.92/`の原稿と保存済みv0.92 PDFは不変のRelease evidence。新layoutは現行版へ適用し、0.92は従来layout / bookmark規則を保持する。旧SHAは`validate_release_docs.py`が検査する。

```sh
python3 tools/generate_manual_pdfs.py --version 0.92 --output-dir build/manuals-v092-repro
cmp docs/Cala-Pokecom-BASIC-v0.92-Install-Manual-ja.pdf build/manuals-v092-repro/Cala-Pokecom-BASIC-v0.92-Install-Manual-ja.pdf
cmp docs/Cala-Pokecom-BASIC-v0.92-System-Manual-ja.pdf build/manuals-v092-repro/Cala-Pokecom-BASIC-v0.92-System-Manual-ja.pdf
cmp docs/Cala-Pokecom-BASIC-v0.92-Programming-Reference-ja.pdf build/manuals-v092-repro/Cala-Pokecom-BASIC-v0.92-Programming-Reference-ja.pdf
```

v0.92固有の過去page数・QA receiptは[Archive](v0.92-manual-pdf-generation.md)に保存する。immutable原稿・PDFを新規約へ合わせて書き換えない。public同期・tag・Release publicationは生成とは別のRelease工程。
