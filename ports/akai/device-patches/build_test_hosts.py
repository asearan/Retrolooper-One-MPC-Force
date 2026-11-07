#!/usr/bin/env python3
"""Cross-build isolated ARM test harnesses for both actual effects and optional shims."""
import json,os,subprocess
from pathlib import Path
PORT=Path(__file__).resolve().parents[1]
OUT=PORT/'catalog-submission/portable-review/arm-tests'
ZIG='/tmp/zig-macos-aarch64-0.13.0/zig'
env={**os.environ,'ZIG_GLOBAL_CACHE_DIR':'/tmp/retrolooper-portable-zig','ZIG_LOCAL_CACHE_DIR':'/tmp/retrolooper-portable-zig-local'}
flags=[ZIG,'cc','-target','arm-linux-gnueabihf.2.28','-mcpu=cortex_a17','-O2','-UNDEBUG']
def run(args):subprocess.run(list(map(str,args)),check=True,env=env)
for device,build,control,uid,version in (
    ('MPC One+',PORT,PORT/'control',0x526c4f31,json.loads((PORT/'vst.json').read_text())['version']),
    ('Akai Force',PORT/'force/workspace',PORT/'force/workspace/control',0x526c4631,json.loads((PORT/'force/profile.json').read_text())['version']),
):
    folder=OUT/device;folder.mkdir(parents=True,exist_ok=True)
    cfg=json.loads((build/'params.json').read_text())['params']
    items={p['key']:i for i,p in enumerate(cfg)}
    wanted=('bars','capture_source','pad_bank_enabled','pad_press_01','sequence_info','status','dub')
    if 'record_offset' in items:wanted+=('record_offset',)
    (folder/'portable-test-params.h').write_text('\n'.join([f'#define P_UID {uid}',f'#define P_VERSION {version}']+
       [f'#define P_{p.upper()} {items[p]}' for p in wanted])+'\n')
    run([*flags,'-I'+str(folder),PORT/'tests/PortableArmHost.c','-ldl','-lm','-o',folder/'MPC'])
    run([*flags,control/'BridgePassThroughTest.c','-ldl','-o',folder/'UnsupportedHost'])
    run([*flags,'-shared','-fPIC',control/'FakeAlsa.c','-o',folder/'fake_alsa.so'])
    print('Isolated ARM fixtures:',folder)
