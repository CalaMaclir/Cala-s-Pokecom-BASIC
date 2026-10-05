# 公開sourceのbuildと検証

対象はPicoCalc + Pico 2 W、Pico SDK 2.3.1です。src/include/CMakeLists/third_partyは正式development sourceと同じblobです。Git履歴と公開CIは独立しています。

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -G Ninja -DPICO_BOARD=pico2_w
cmake --build build
# host試験用にSDKをpico-sdkへ用意（btstackとtinyusbを含む）
cmake -S tests/host -B build-host -G Ninja
cmake --build build-host --parallel 2
ctest --test-dir build-host --output-on-failure --output-junit test-results.xml
python3 tools/validate_manual_reference.py
python3 tools/validate_release_docs.py
```

native C++17、CMake/Ninja、Python3、ccacheが必要です。assertとsanitizersを維持し、全host regressionを逐次実行します。性能reportとcorrectnessは別に扱います。元private workflowの2つのtopology検査は公開用の明示したsnapshot fixtureを確認し、公開workflowの検証と混同しません。製品・意味・evidenceの検査は変更していません。

受入情報を別Firmware変更へ引き継がないでください。manualの変更は[生成手順](manual-pdf-generation.md)に従いPDFとmanifestを更新します。正式公開のsource/asset対応は[公開record](../release/v0.94-release-checklist.md)を参照してください。
