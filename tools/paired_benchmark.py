"""Serial, CPU-pinned, case/metric-paired host measurements; never discard samples."""
import ctypes, json, os, platform, shutil, subprocess, tempfile, time
from pathlib import Path
from performance_policy import positive, same

METRICS = ("compile", "vm", "cache_miss", "cache_hit")
TRIALS = 9

def environment():
    original = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    if original:
        os.sched_setaffinity(0, {original[-1]})
    def read(path):
        try:
            return Path(path).read_text().strip()
        except OSError:
            return None
    return {
        "platform": platform.platform(),
        "original_affinity": original,
        "measurement_affinity": sorted(os.sched_getaffinity(0)) if original else [],
        "cpu_model": next((line.split(":", 1)[1].strip()
            for line in (read("/proc/cpuinfo") or "").splitlines() if line.startswith("model name")), None),
        "governor": read(f"/sys/devices/system/cpu/cpu{original[-1] if original else 0}/cpufreq/scaling_governor"),
        "load_start": list(os.getloadavg()) if hasattr(os, "getloadavg") else [],
        "policy": "one fixed available logical CPU; serial commands; nine alternating revision rounds per case/metric; all samples retained",
        "external_host_load_controlled": False,
        "process_layout_policy": "child ASLR disabled; equal-length binary paths; canonical argv[0]; accepted/current mapped addresses checked; no fallback",
    }

def repetitions(case, metric):
    # Fixed across revisions. The BAS loops/resolution/precision are untouched.
    if metric == "compile":
        return 20000
    if metric == "vm":
        if case in ("examples/v093/performance/for-empty.bas", "examples/v093/performance/for-nested.bas",
                    "examples/v093/performance/for-step2.bas", "examples/v093/performance/for-negative.bas"):
            return 16  # Million-iteration research fixtures; equal for A/B/C.
        return 2 if "fractal" in case or "picocalc" in case else 1000
    return 10000

def fixed_address_layout():
    # Child-only Linux personality: fail closed if the runner cannot establish it.
    libc = ctypes.CDLL(None, use_errno=True)
    libc.personality.argtypes = [ctypes.c_ulong]
    libc.personality.restype = ctypes.c_int
    current = libc.personality(0xffffffff)
    if current == -1 or libc.personality(current | 0x40000) == -1:
        raise OSError(ctypes.get_errno(), "cannot disable ASLR for Host timing child")


def process_layout(pid, executable):
    personality = int(Path(f"/proc/{pid}/personality").read_text().strip(), 16)
    assert personality & 0x40000, "timing child ASLR remains enabled"
    layout = {"personality": personality, "image": [], "heap": [],
              "anonymous_writable": [], "stack": []}
    for line in Path(f"/proc/{pid}/maps").read_text().splitlines():
        fields = line.split(maxsplit=5)
        name = fields[5] if len(fields) == 6 else ""
        region = fields[:3]  # address, permissions and offset only; no private paths
        if name == executable: layout["image"].append(region)
        elif name == "[heap]": layout["heap"].append(region)
        elif name == "[stack]": layout["stack"].append(region)
        elif not name and "w" in fields[1]: layout["anonymous_writable"].append(region)
    assert layout["image"], "timing executable map was not captured"
    return layout


def measure(binaries, case_names):
    # Equal-length execution paths and argv[0] keep initial process layout fair.
    # These are byte-identical copies of the measured binaries, not new builds.
    with tempfile.TemporaryDirectory(prefix="cpb-timing-") as directory:
        copies = {}
        for index, (name, binary) in enumerate(binaries.items()):
            source = Path(binary).resolve()
            # The Classic wrapper execs its sibling unchanged for --paired-case.
            # Copy that same linked VM directly so relocation preserves its behavior.
            if source.name == "classic-benchmark":
                source = source.with_name("performance-benchmark")
            assert source.is_file(), source
            target = Path(directory) / f"rev{index:06d}"
            shutil.copy2(source, target)
            assert target.read_bytes() == source.read_bytes()
            copies[name] = str(target)
        return _measure(copies, case_names)


def _measure(binaries, case_names):
    """case_names maps each source path to the revisions able to execute it."""
    runs = {name: [{"cases": []}] for name in binaries}
    schedule = []
    for case, names in case_names.items():
        processes = {}
        results = {}
        try:
            for name in names:
                process = subprocess.Popen(["cpb-performance-benchmark", "--paired-case", case],
                    executable=binaries[name], preexec_fn=fixed_address_layout,
                    text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=1)
                processes[name] = process
                line = process.stdout.readline()
                if not line:
                    raise RuntimeError(f"{name} {case}: setup exited {process.poll()}")
                results[name] = json.loads(line)
                assert results[name]["name"] == case
                results[name]["process_layout"] = process_layout(process.pid, binaries[name])
            if "Accepted 3AB" in names and "Current" in names and Path(binaries["Accepted 3AB"]).read_bytes()==Path(binaries["Current"]).read_bytes():
                for key in ("image", "heap", "anonymous_writable", "stack"):
                    assert results["Accepted 3AB"]["process_layout"][key] == results["Current"]["process_layout"][key], (case, key, "frozen binary process layouts differ")
            for metric in METRICS:
                count = repetitions(case, metric)
                # One entire unmeasured batch per revision/metric establishes a
                # sustained warm workload; each recorded batch adds three calls.
                for name in names:
                    p = processes[name]
                    p.stdin.write(f"{metric} {count} 1\n"); p.stdin.flush()
                    assert json.loads(p.stdout.readline()) == {"warmup": True}
                    results[name][metric+"_raw_us"] = []
                for trial in range(TRIALS):
                    order = list(names)
                    if trial % 2:
                        order.reverse()
                    for name in order:
                        p = processes[name]
                        started = time.time()
                        p.stdin.write(f"{metric} {count} 0\n"); p.stdin.flush()
                        line = p.stdout.readline()
                        if not line:
                            raise RuntimeError(f"{name} {case}/{metric}: exited {p.poll()}")
                        value = json.loads(line)["us"]
                        positive(value)
                        results[name][metric+"_raw_us"].append(value)
                        schedule.append({"case": case, "metric": metric, "trial": trial,
                            "revision": name, "repetitions": count, "us": value,
                            "start_unix": started, "end_unix": time.time(),
                            "load": list(os.getloadavg()) if hasattr(os, "getloadavg") else []})
            for name in names:
                results[name]["repetitions"] = {m: repetitions(case, m) for m in METRICS}
                results[name]["vm_repetitions"] = repetitions(case, "vm")
                runs[name][0]["cases"].append(results[name])
        finally:
            evidence=Path('build/paired-partial-evidence.json')
            evidence.parent.mkdir(parents=True,exist_ok=True)
            evidence.write_text(json.dumps({'runs':runs,'schedule':schedule,'active_case':case,'active_results':results},indent=2)+'\n')
            for p in processes.values():
                p.stdin.close()
            for p in processes.values():
                status = p.wait()
                if status:
                    raise RuntimeError(f"benchmark process failed: {case}, exit {status}")
    return runs, schedule

def self_test(binary):
    """Exercise setup, all commands, warmup replies and orderly EOF without timing assertions."""
    p = subprocess.Popen([str(Path(binary).resolve()), "--paired-case",
        "examples/stage3/arithmetic-classic.bas"], text=True,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=1)
    case = json.loads(p.stdout.readline())
    same(case["output_hash"], 3882862080, "protocol output")
    same(case["dispatches"], 20010, "same-version protocol dispatch")
    for metric in METRICS:
        for warmup in (1, 0):
            p.stdin.write(f"{metric} 2 {warmup}\n"); p.stdin.flush()
            reply = json.loads(p.stdout.readline())
            if warmup:same(reply, {"warmup": True}, "warmup reply")
            else:positive(reply["us"])
    p.stdin.close()
    assert p.wait() == 0
    print("Paired measurement protocol, PROFILE separation and EOF: PASS")

if __name__ == "__main__":
    import sys
    assert len(sys.argv) == 3 and sys.argv[1] == "--self-test"
    self_test(sys.argv[2])


