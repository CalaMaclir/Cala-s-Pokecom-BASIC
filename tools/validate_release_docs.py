"""Check current manuals/spec constants and hashes without claiming visual or device acceptance."""
from pathlib import Path
import hashlib,json,re
ROOT=Path(__file__).resolve().parents[1]
def main():
    il=(ROOT/"include/il.hpp").read_text()
    for name,value in {"kMaxUserFunctions":32,"kMaxFunctionLocals":128,"kMaxFunctionParameters":8,
                       "kMaxFunctionCallDepth":16,"kMaxOps":1536,"kStringPoolSize":6144,"kMaxSymbols":64}.items():
        assert re.search(r"\b"+name+r"\s*=\s*"+str(value)+r"\s*;",il),name
    assert 'set(RMB_VERSION "0.92"' in (ROOT/"CMakeLists.txt").read_text()
    manifest=json.loads((ROOT/"docs/manuals-manifest.json").read_text())
    assert len(manifest["manuals"])==3 and manifest["version"]=="0.92"
    assert hashlib.sha256((ROOT/manifest["generator"]).read_bytes()).hexdigest()==manifest["generator_sha256"]
    index=(ROOT/"docs/manual-ja.md").read_text()
    assert index.startswith("# Cala's Pokecom BASIC Version 0.92 日本語マニュアル\n")
    for readme in ("README.md","README.en.md"):
        text=(ROOT/readme).read_text()
        assert "docs/manual-ja.md" in text,readme
        for manual in manifest["manuals"]:
            assert f'({manual["output"]})' in text,(readme,manual["output"])
    for target in re.findall(r"\]\(([^)]+)\)",index):
        assert (ROOT/"docs"/target).is_file(),("manual index link",target)
    for manual in manifest["manuals"]:
        assert f'({Path(manual["output"]).name})' in index,manual["output"]
    for manual in manifest["manuals"]:
        source=ROOT/manual["source"];s=source.read_text()
        assert "## Version 0.92\n" in s
        for obsolete in ("実機確認待ち","root-only","ルートディレクトリのみ","Version 0.91","この段階はDraft"):
            assert obsolete not in s,(source,obsolete)
        for key,digest in (("source","source_sha256"),("output","output_sha256")):
            assert hashlib.sha256((ROOT/manual[key]).read_bytes()).hexdigest()==manual[digest]
        assert (ROOT/manual["output"]).read_bytes().startswith(b"%PDF-")
    system=(ROOT/"docs/system-manual-ja.md").read_text()
    for phrase in ("256 KiB","00001  REM","先頭行が10","format 3","version 2","最大79文字"):
        assert phrase in system,phrase
    print("Current manual versions, limits, index/README links, source/PDF/generator hashes: PASS (visual review is separate)")
if __name__=="__main__":main()
