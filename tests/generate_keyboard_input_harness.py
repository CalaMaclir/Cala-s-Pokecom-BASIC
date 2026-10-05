"""Build a harness using production function bodies, never a duplicate loop."""
from pathlib import Path
import sys

source = Path("src/platform/picocalc/platform.cpp").read_text()
def function(signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end]

functions = [
    function("void begin_command_input()"),
    function("void end_command_input()"),
    function("int read_command_source("),
    function("int decode_terminal_key("),
    "namespace { bool command_key_repeat = false; }",
    function("bool last_key_repeat()"),
    function("int get_char_timeout("),
    function("int get_char()"),
]
template = Path("tests/keyboard_input_progress_test.cpp").read_text()
Path(sys.argv[1]).write_text(
    template.replace("// @PRODUCTION_FUNCTIONS@", "\n".join(functions)))
