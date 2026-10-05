"""Hashes/toolchain/flags for the three actual ARM images, separate from timing."""
import argparse, hashlib, json, platform, subprocess, tempfile
from pathlib import Path

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',required=True)
    p.add_argument('builds',nargs=3);a=p.parse_args();result={}
    for name,directory in zip(('Stage 4','Normal','IntFOR experimental'),a.builds):
        root=Path(directory);elf=root/'CPokecombasic.elf';uf2=root/'CPokecombasic.uf2'
        with tempfile.TemporaryDirectory() as temp:
            text=Path(temp)/'text.bin';load=Path(temp)/'load.bin'
            subprocess.run(['arm-none-eabi-objcopy','--dump-section','.text='+str(text),str(elf),str(Path(temp)/'copy.elf')],check=True)
            subprocess.run(['arm-none-eabi-objcopy','-O','binary',str(elf),str(load)],check=True)
            load_size=load.stat().st_size
            hashes={k:hashlib.sha256(v.read_bytes()).hexdigest() for k,v in
                (('elf',elf),('uf2',uf2),('text_section',text),('load_image',load))}
        commands=json.loads((root/'compile_commands.json').read_text())
        flags=[e.get('command',e.get('arguments')) for e in commands if e['file'].endswith('/src/core/vm.cpp')]
        assert len(flags)==1
        result[name]={'sha256':hashes,'load_image_bytes':load_size,'vm_compile_command':flags[0],
            'compiler':subprocess.check_output(['arm-none-eabi-g++','--version'],text=True).splitlines()[0],
            'pico_sdk':'2.3.1','sdk_sha':subprocess.check_output(['git','-C','pico-sdk','rev-parse','HEAD'],text=True).strip(),
            'host_platform':platform.platform(),'experiment':name=='IntFOR experimental'}
    Path(a.output).write_text(json.dumps(result,indent=2)+'\n')
if __name__=='__main__':main()
