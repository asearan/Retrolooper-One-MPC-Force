#!/usr/bin/env python3
"""Verify every helper instruction/type guard against the copied Force ELF."""
import hashlib,importlib.util,json,os,re,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=json.loads((ROOT/'diagnostics/native-profile-candidate.json').read_text())
os.environ['RETROLOOPER_HOST_ELF']=sys.argv[1] if len(sys.argv)>1 else '/tmp/retrolooper-force-3.10.elf'
spec=importlib.util.spec_from_file_location('elf',ROOT.parent/'diagnostics/native-elf.py');e=importlib.util.module_from_spec(spec);spec.loader.exec_module(e)
assert hashlib.sha256(e.B).hexdigest()==p['image_sha256'];assert bytes.fromhex(p['build_id']) in e.B[:1024]
checks=[]
for file in ['TrackBridge.h','EditorTrack.h','EditorFollow.h','BounceClip.h']:
 s=(ROOT/'workspace/control'/file).read_text()
 for a,relative,b in re.findall(r'word_is\(native_base\+(0x[0-9a-f]+),(native_base\+)?(0x[0-9a-f]+)\)',s):
  actual=e.word(int(a,16));expected=int(b,16);assert actual==expected,(file,a,hex(actual),b)
  checks.append(dict(file=file,address=a,expected=b,relative=bool(relative)))
assert len(checks)==36,len(checks)
report=dict(image_sha256=p['image_sha256'],build_id=p['build_id'],guard_count=len(checks),checks=checks,status='PASS')
(ROOT/'diagnostics/firmware-guard-validation.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS:',len(checks),'exact native code/type guards; Force build ID and SHA256 match')
