"""Execute production PWM transport/service code with hardware and decoder stubs."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


source = (ROOT / "src/platform/picocalc/audio.cpp").read_text()
signatures = [
    "void prepare_pwm_for_start()",
    "void begin_pwm_bias_ramp(bool rising)",
    "void halt_pwm_output()",
    "bool __not_in_flash_func(activate_next_buffer)()",
    "void __not_in_flash_func(write_pwm_word)(",
    "void __not_in_flash_func(pwm_wrap_handler)()",
    "void start_ready_pwm()",
    "void reset_output()",
    "void audio_service()",
    "void audio_stop()",
    "void audio_set_key_click(",
    "void audio_key_click()",
]
template = (ROOT / "tests/audio_output_lifecycle_test.cpp").read_text()
idle_policy = source[source.index("constexpr std::uint32_t kIdlePowerOffDelayMs"):
                     source.index("volatile bool pwm_irq_active")]
with tempfile.TemporaryDirectory(prefix="cpb-audio-output-") as folder:
    cpp = Path(folder) / "audio-output.cpp"
    exe = Path(folder) / "audio-output-test"
    cpp.write_text(template.replace("// @IDLE_POLICY@", idle_policy)
                   .replace("// @PRODUCTION_FUNCTIONS@",
                                   "\n".join(function(source, s) for s in signatures)))
    subprocess.run(["g++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-UNDEBUG", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I" + str(ROOT / "include"), str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"},
                   timeout=10, check=True)
print("Production PWM 10-second grace / 100ms bias ramps / endpoint latch / ramp reversal / full audio drain / timer wrap / stop: PASS")
