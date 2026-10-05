"""Write reviewable identity and hashes for the firmware ZIP."""
import hashlib
import json
import os
import re
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path

def accepted_product_matches(root, record):
    if record.get("schema_version") != 1 or record.get("status") != "USER_ACCEPTED":
        return False
    paths = {"CMakeLists.txt"}
    for directory in ("src", "include", "third_party"):
        paths.update(str(p.relative_to(root)) for p in (root/directory).rglob("*") if p.is_file())
    if (root/"pico_sdk_import.cmake").is_file():
        paths.add("pico_sdk_import.cmake")
    expected = record.get("product_blobs", {})
    if set(expected) != paths:
        return False
    for path in paths:
        if not (root/path).is_file():
            return False
        data = (root/path).read_bytes()
        blob = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
        if blob != expected[path]:
            return False
    return True


def sdk_identity(sdk):
    def git(*args):
        return subprocess.check_output(['git','-C',str(sdk),*args],text=True).strip()
    try:
        commit=git('rev-parse','HEAD')
        dirty=bool(git('status','--porcelain','--untracked-files=all','--ignore-submodules=none'))
        # Preserve each leading state marker; '+'/'-'/'U' are not accepted.
        lines=subprocess.check_output(['git','-C',str(sdk),'submodule','status','--recursive'],text=True).splitlines()
        submodules={}
        for line in lines:
            if not line:continue
            if line[0]!=' ':dirty=True
            fields=line[1:].split()
            if len(fields)<2:return {'verified':False}
            submodules[fields[1]]=fields[0]
        return {'verified':not dirty,'commit':commit,'submodules':submodules}
    except (subprocess.CalledProcessError,FileNotFoundError):
        return {'verified':False}


def main():
    root=Path(__file__).resolve().parents[1]
    uf2=root/"build/CPokecombasic.uf2"
    cases=list(ET.parse(root/"build-host/test-results.xml").getroot().iter("testcase"))
    counts={"tests":len(cases),"failures":sum(c.find("failure") is not None for c in cases),
            "errors":sum(c.find("error") is not None for c in cases),
            "skipped":sum(c.find("skipped") is not None for c in cases)}
    assert counts["tests"]>=47 and counts["failures"]==counts["errors"]==counts["skipped"]==0,counts
    codegen=json.loads((root/"build/local-numeric-codegen.json").read_text())
    assert codegen["pass"] and not codegen["fma_instructions"]
    from performance_policy import load_summary, same, ACCEPTED_SHA
    mode=os.environ.get("PERFORMANCE_MODE","off")
    performance=load_summary(root/"build",mode)
    same(json.loads((root/"build/performance-status.json").read_text()),performance,"performance summary")
    if mode=="report":
        evidence_name="stage5-evidence.json" if "v093_stage5ab_compile_vm_cache" in performance.get("reports",{}) else "v093-evidence.json" if "v093_stage1_compile_vm_cache" in performance.get("reports",{}) else "stage3-evidence.json"
        evidence=json.loads((root/"build"/evidence_name).read_text())
        same(evidence["checkout_sha"],subprocess.check_output(["git","rev-parse","HEAD"],cwd=root,text=True).strip(),"measured checkout")
    inventory=json.loads((root/"build-host/test-inventory.json").read_text())
    inventory_tests=inventory.get("tests",[])
    assert len(inventory_tests)==counts["tests"],(len(inventory_tests),counts)
    assert {c.attrib["name"] for c in cases}=={t["name"] for t in inventory_tests}
    metrics=json.loads((root/"build-host/host-metrics.json").read_text())
    checkout=subprocess.check_output(["git","rev-parse","HEAD"],cwd=root,text=True).strip()
    event=os.environ.get("GITHUB_EVENT_NAME","")
    cache=(root/"build/CMakeCache.txt").read_text()
    import re
    firmware_version=re.search(r'^RMB_VERSION:STRING=(.+)$',cache,re.M).group(1)
    manuals=json.loads((root/"docs/manuals-manifest.json").read_text())
    addendum=root/"docs/development/archive/v0.94/v0.94-stage1-2-files-system-information.md"
    development_manuals = firmware_version == "0.94" and manuals["version"] == "0.93" and addendum.is_file()
    assert manuals["version"]==firmware_version or development_manuals
    for manual in manuals["manuals"]:
        for key,hash_key in (("source","source_sha256"),("output","output_sha256")):
            assert hashlib.sha256((root/manual[key]).read_bytes()).hexdigest()==manual[hash_key]
    manifest={"firmware_version":firmware_version,"event":event,"checkout_sha":checkout,
              "stage2_baseline_sha":"a4b54b596a674dfa231c45743ff977376d8b62d4",
              "stage3_components":["3A Files UI","3B compiler/VM performance","3C release preparation"],
              "accepted_stage3_sha":ACCEPTED_SHA,
              "manuals":manuals,
              "full_validation":os.environ.get("FULL_VALIDATION","false"),
              "repository":os.environ["GITHUB_REPOSITORY"],
              "branch_head_sha":os.environ["CPB_BRANCH_HEAD_SHA"],
              "tested_merge_sha":checkout if event=="pull_request" else None,
              "baseline_sha":"35367d7b6156fce92fbebbee7407261041e7af28",
              "workflow_run_id":os.environ["GITHUB_RUN_ID"],
              "build_number":os.environ["GITHUB_RUN_NUMBER"],
              "run_attempt":os.environ["GITHUB_RUN_ATTEMPT"],
              "artifact_name":os.environ["CPB_ARTIFACT_NAME"],
              "board":"pico2_w","pico_sdk":"2.3.1",
              "host_results":counts,
              "local_numeric_codegen":{"pass":codegen["pass"],"fma_count":len(codegen["fma_instructions"])},
              "performance_report":performance,
              "performance_gates":performance.get("reports",{"classic":"NOT_RUN","full_compile_vm_cache":"NOT_RUN"}),
              "uf2_path":"build/CPokecombasic.uf2",
              "uf2_bytes":uf2.stat().st_size,
              "uf2_sha256":hashlib.sha256(uf2.read_bytes()).hexdigest(),
              "host_metrics":metrics}
    if (root/"build/v093-evidence.json").is_file():
        manifest["development_stage"]="v0.93 Stage 1 Structured BASIC Performance"
        manifest["development_baseline_sha"]="1ee0477ca8b157e8e953cb4fecf48291705f3731"
    if (root/"docs/development/archive/v0.93/v0.93-stage3a3b-math-intops.md").is_file():
        manifest["development_stage"]="v0.93 Stage 3A/3B Math Functions and Integer Operators"
        manifest["development_baseline_sha"]="84b5fbc66ae9ce99995233abd60c117e96b06b78"
        manifest["stage3_components"]=["3A Math Functions", "3B Integer / Bit Operators"]
        manifest["compiled_cache_format"]=5
        manifest["numeric_storage"]="float32"
        manifest["log_compatibility"]="LOG base 10; use LN for the v0.92 LOG behavior"
    if (root/"docs/development/archive/v0.93/v0.93-stage3c3d3e-control-data-select.md").is_file():
        manifest["development_stage"]="v0.93 Stage 3C/3D/3E Control, DATA and SELECT"
        manifest["development_baseline_sha"]="bf3aa3191e473f9ef8552985ef60936f7d551648"
        manifest["stage3_components"]=["3A Math Functions", "3B Integer / Bit Operators",
                                       "3C EXIT FOR / EXIT DO", "3D DATA / READ / RESTORE",
                                       "3E SELECT CASE"]
        manifest["compiled_cache_format"]=6
    if (root/"docs/development/archive/v0.93/v0.93-stage4-hardware-stability.md").is_file():
        manifest["development_stage"]="v0.93 Stage 4 PicoCalc Hardware Stability"
        manifest["development_baseline_sha"]="29991481e38b22e3e4d8b8b4004b953c96d61c25"
        manifest["hardware_acceptance"]="PENDING USER HARDWARE TESTS"
        manifest["keyboard_bios_update_required"]=False
    if (root/"docs/development/archive/v0.93/v0.93-stage5ab-performance-integer-research.md").is_file():
        manifest["development_stage"]="v0.93 Stage 5A/5B Integer Research and VM Performance"
        manifest["development_baseline_sha"]="d2449c38971d9b04c777252ffe29ecc60bb6cffa"
        manifest["integer_for_experiment"]=False
        manifest["compiled_cache_format"]=6
        manifest["stage5c_integer_type"]="NOT IMPLEMENTED; DEFER pending RP2350 measurements"
    if manuals["version"] == "0.93" and (root/"docs/release/archive/v0.93/v0.93-release-checklist.md").is_file():
        manifest["development_stage"]="v0.93 Stage 6 Documentation and Release Candidate"
        manifest["development_baseline_sha"]="8a774c112bdcae4a5c57e8f6ffe150bf3f5153c0"
        manifest["hardware_acceptance"]="IMPLEMENTATION STAGES 1-5D USER ACCEPTED; NO NEW HARDWARE TEST IN STAGE 6"
    if (root/"docs/development/archive/v0.93/v0.93-additional-a1-wifi-retry-settle.md").is_file():
        manifest["development_stage"]="v0.93 Additional A-1 Wi-Fi Retry and Profile Connection"
        manifest["development_baseline_sha"]="fc978558110b73367982f34555c2ee4df9c484cf"
        manifest["hardware_acceptance"]="PENDING USER A-1 HARDWARE TESTS"
        manifest["wifi_radio_settle_timeout_ms"]=2000
        manifest["wifi_add_mode"]="CONNECT_THEN_SAVE"
        manifest["wifi_scan_completion_mode"]="OBSERVE_THEN_DRAIN"
        manifest["wifi_scan_drain_timeout_ms"]=10000
        manifest["ntp_failure_notification"]="MENU_WARNING_KEEP_WIFI"
    if development_manuals:
        manifest["development_stage"]="v0.94 Stage 1+2 Files Input and System Information"
        manifest["development_baseline_sha"]="840129a5601578d10cf71c2059a035d7c3bcbc7b"
        manifest["stage3_components"]=["Stage 1 Playback input ownership", "Stage 2 Files UI", "System Information diagnostics"]
        manifest["hardware_acceptance"]="PENDING USER HARDWARE TESTS"
        manifest["release_candidate"]=False
        manifest["manuals_status"]="v0.93 baseline plus v0.94 development addendum"
        manifest["development_addendum"]={"path":str(addendum.relative_to(root)),"sha256":hashlib.sha256(addendum.read_bytes()).hexdigest()}
    stage3=root/"docs/development/archive/v0.94/v0.94-stage3-image-io.md"
    if development_manuals and stage3.is_file():
        manifest["development_stage"]="v0.94 Stage 3 Image I/O"
        manifest["development_baseline_sha"]="9e6cc7e0cbf996305a8ce03529be25f1743e63d3"
        manifest["stage3_components"]=["LOADIMAGE", "SAVEIMAGE / SAVE IMAGE compatibility", "Program / image I/O separation"]
        manifest["manuals_status"]="v0.93 baseline with v0.94 Stage 3 image I/O supplement"
        manifest["development_addendum"]={"path":str(stage3.relative_to(root)),"sha256":hashlib.sha256(stage3.read_bytes()).hexdigest()}
    stage46=root/"docs/development/archive/v0.94/v0.94-stage4-6-diagnostics-editor.md"
    if development_manuals and stage46.is_file():
        manifest["development_stage"]="v0.94 Stage 4-6 Error / Editor / Diagnostics"
        manifest["development_baseline_sha"]="2b6853c1788fe143f736782f437c5b85a72f7dd9"
        manifest["stage4_6_components"]=["Error Context / Last Error", "Structured auto-indent / Error-to-Editor", "Read-only Diagnostics / USB Serial Report"]
        manifest["manuals_status"]="v0.93 baseline with v0.94 Stage 3 and Stage 4-6 supplements"
        manifest["development_addendum"]={"path":str(stage46.relative_to(root)),"sha256":hashlib.sha256(stage46.read_bytes()).hexdigest()}
    stage789=root/"docs/development/archive/v0.94/v0.94-stage7-9-program-protection.md"
    if development_manuals and stage789.is_file():
        manifest["development_stage"]="v0.94 Stage 7-9 Build / Daily BASIC / Program Protection"
        manifest["development_baseline_sha"]="5fe98847bf18b867c4f938c6faa5c03b735f6ea6"
        manifest["stage7_9_components"]=["Manual-only keyboard recovery workflow", "Daily string / RTC functions",
            "Shared unsaved changes guard", "INFO / LASTERROR"]
        manifest["manuals_status"]="v0.93 baseline with v0.94 Stage 1-9 development supplements"
        manifest["development_addendum"]={"path":str(stage789.relative_to(root)),"sha256":hashlib.sha256(stage789.read_bytes()).hexdigest()}
    if firmware_version == "0.94" and manuals["version"] == "0.94":
        stage1011=root/"docs/development/archive/v0.94/v0.94-stage10-11-stability-release.md"
        assert stage1011.is_file()
        manifest["development_stage"]="v0.94 Stage 10-11 Stability / Release Candidate"
        manifest["development_baseline_sha"]="5d59da6d114343356c729616c1d7815548d43a1d"
        manifest["manuals_status"]="v0.94 current manuals; immutable historical v0.92 retained"
        manifest["release_candidate"]=True
        manifest["release_ready"]=False
        manifest["hardware_acceptance"]="STAGES 1-9 USER ACCEPTED; FINAL SOAK / SMOKE PENDING"
        manifest["release_gate"]="FINAL_HARDWARE_SOAK_AND_SMOKE_PENDING"
        manifest["development_addendum"]={"path":str(stage1011.relative_to(root)),"sha256":hashlib.sha256(stage1011.read_bytes()).hexdigest()}
        evidence=json.loads((root/"build/stability-evidence.json").read_text())
        assert evidence["host_stability_pass"] and evidence["hardware_status"] == "NOT_RUN"
        manifest["stability_evidence"]=evidence
    acceptance_path=root/f"docs/release/v{firmware_version}-hardware-acceptance.json"
    if acceptance_path.is_file():
        record=json.loads(acceptance_path.read_text())
        matched=accepted_product_matches(root,record)
        cache=(root/'build/CMakeCache.txt').read_text()
        sdk_path=re.search(r'^PICO_SDK_PATH(?::[^=]+)?=(.+)$',cache,re.M)
        sdk=Path(sdk_path.group(1)) if sdk_path else root/'pico-sdk'
        actual_sdk=sdk_identity(sdk)
        manifest['pico_sdk_identity']=actual_sdk
        sdk_matched=(actual_sdk.get('verified') is True
            and actual_sdk.get('commit')==record.get('pico_sdk_commit')
            and actual_sdk.get('submodules')==record.get('pico_sdk_submodules'))
        manifest["hardware_acceptance_record"]={
            "path":str(acceptance_path.relative_to(root)),
            "sha256":hashlib.sha256(acceptance_path.read_bytes()).hexdigest(),
            "accepted_source":record.get("accepted_source"),
            "evidence_url":record.get("evidence_url"),
            "product_match":matched,
            "sdk_match":sdk_matched,
            "publication_build_device_test":"NOT_RUN"}
        if matched and sdk_matched and record.get("version")==firmware_version and record.get("pico_sdk")==manifest["pico_sdk"]:
            manifest["release_candidate"]=False
            manifest["release_ready"]=True
            manifest["hardware_acceptance"]="FINAL CANDIDATE USER ACCEPTED; PRODUCT SOURCE MATCHED; NO NEW PUBLICATION DEVICE TEST"
            manifest["release_gate"]="OWNER_ACCEPTANCE_WITH_MATCHING_PRODUCT_SOURCE"
    text=json.dumps(manifest,ensure_ascii=False,indent=2)+"\n"
    (root/"build/firmware-manifest.json").write_text(text)
    print(text)
if __name__=="__main__":main()
