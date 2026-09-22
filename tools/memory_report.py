"""Report linked SRAM usage and ARM sizeof probes without running firmware."""
import subprocess
import sys
from pathlib import Path

build = Path(sys.argv[1])
elf = build / 'retrominibasic_picocalc.elf'
def run(*args):
    return subprocess.check_output(args, text=True)

sections = run('arm-none-eabi-size', '-A', str(elf))
print(sections)
sizes = {}
for line in sections.splitlines():
    parts = line.split()
    if len(parts) >= 3 and parts[0].startswith('.'):
        sizes[parts[0]] = int(parts[1])
print('DATA_PLUS_BSS_BYTES', sizes.get('.data', 0) + sizes.get('.bss', 0))
symbols = run('arm-none-eabi-nm', '-S', '--size-sort', '--radix=d', '-C', str(elf))
print('\nLargest writable symbols (bytes):')
print('\n'.join([line for line in symbols.splitlines() if len(line.split()) > 3 and 0x20000000 <= int(line.split()[0]) < 0x20082000][-40:]))
print('\nLinker heap/stack boundaries (hex addresses; not runtime high-water marks):')
print('\n'.join(line for line in run('arm-none-eabi-nm', '-n', str(elf)).splitlines() if any(k in line for k in ('__Stack', '__Heap', '__end__', '__bss_end__'))))
print('\nARM object sizes (bytes):')
print('\n'.join(line for line in run('arm-none-eabi-nm', '-S', '--size-sort', '--radix=d', str(build / 'memory_sizes.o')).splitlines() if 'memory_size_' in line))
print('\nCompiler stack usage:')
for path in build.rglob('*.su'):
    if any(name in path.name for name in ('vm.cpp', 'basic_compiler.cpp', 'repl.cpp', 'xmodem.cpp', 'ymodem.cpp', 'serial_transfer.cpp', 'program_store.cpp', 'storage.cpp', 'usb_msc.cpp')):
        print(path.name)
        print(path.read_text())
