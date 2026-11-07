#!/usr/bin/env python3
"""Build Force-only helper and isolated ARM fixtures; never install or restart."""
import argparse,os,subprocess,sys
from pathlib import Path
import native_control
ROOT=Path(__file__).resolve().parent
WORK=ROOT/'workspace'
def run(args,**kw):print('+',' '.join(map(str,args)),flush=True);subprocess.run(list(map(str,args)),check=True,**kw)
def main():
 p=argparse.ArgumentParser();p.add_argument('--zig',default='/tmp/zig-macos-aarch64-0.13.0/zig');a=p.parse_args();dest=native_control.generate(WORK)
 env={**os.environ,'ZIG_GLOBAL_CACHE_DIR':str(WORK/'build/zig-cache'),'ZIG_LOCAL_CACHE_DIR':str(WORK/'build/zig-local')}
 common=[a.zig,'cc','-std=c11','-target','arm-linux-gnueabihf.2.28','-mcpu=cortex_a17','-O2','-fvisibility=hidden']
 for name in ['TrackBridgeModelTest','EditorTrackModelTest','EditorFollowModelTest','ForceControlTest','BridgePassThroughTest']:
  run([*common,'-UNDEBUG',dest/(name+'.c'),'-ldl','-pthread','-o',dest/(name+'-armv7')],env=env)
 run([*common,'-c','-fPIC',dest/'ButtonBridge.c','-o',dest/'ButtonBridge.o'],env=env)
 run([a.zig,'c++','-target','arm-linux-gnueabihf.2.28','-mcpu=cortex_a17','-O2','-fvisibility=hidden','-shared','-fPIC',
      dest/'NativeClipCall.cpp',dest/'ButtonBridge.o','-pthread','-ldl','-s','-Wl,--no-undefined','-Wl,--version-script='+str(dest/'exports.map'),'-o',dest/'retrolooper_force_controls.so'],env=env)
 run([*common,'-shared','-fPIC','-fvisibility=default',dest/'FakeAlsa.c','-o',dest/'fake_alsa.so'],env=env)
 run([sys.executable,dest/'check_exports.py'])
if __name__=='__main__':main()
