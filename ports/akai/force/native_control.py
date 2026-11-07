"""Generate the Force helper from the shared implementation and exact ABI map.
Protocol parsing and application discovery are Force-specific source files.
This builds files only; no SSH, installation, tracing or native calls.
"""
import json,re,shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parent
SHARED=ROOT.parent
def generate(work):
    profile=json.loads((ROOT/'diagnostics/native-profile-candidate.json').read_text())
    dest=Path(work)/'control';dest.mkdir(exist_ok=True)
    for name in ['ButtonBridge.c','TrackBridge.h','TrackProtocol.h','EditorTrack.h','PadBridge.h','exports.map','check_exports.py','TrackBridgeModelTest.c','EditorTrackModelTest.c','FakeAlsa.c','BridgePassThroughTest.c','native-ui-LICENSE','mpc-vst-plugins-LICENSE']:
        shutil.copy2(SHARED/'control'/name,dest/name)
    force_sources=['ForceRoot.h','PadContext.h','PadProtocol.h','ButtonProtocol.h','EditorFollow.h','ClipProtocol.h','ClipTarget.h','BounceClip.h','ClipPicker.h','NativeClipCall.cpp']
    clip_exports=['retrolooper_force_clip_'+x for x in ('target','enqueue','result')]+['retrolooper_force_picker_begin','retrolooper_force_picker_poll']
    for name in force_sources:shutil.copy2(ROOT/'control'/name,dest/name)
    for path in dest.iterdir():
        if path.suffix not in ['.c','.h','.py','.map']:continue
        if path.name in force_sources:continue
        s=path.read_text().replace('retrolooper_mpc_','retrolooper_force_').replace('0x526c4f31','0x526c4631')
        s=re.sub(r'0x[0-9a-f]+',lambda m:profile['native_addresses'].get(m[0],m[0]),s)
        s=s.replace('0xea9c4659','0xea9c473f')
        if path.name=='TrackBridge.h':
            pattern=r'static const unsigned char build\[20\]=\{[^}]+\};'
            value='static const unsigned char build[20]={'+','.join('0x'+profile['build_id'][i:i+2] for i in range(0,40,2))+'};'
            s,n=re.subn(pattern,value,s);assert n==1
            start=s.index('static void discover_model(void){');end=s.index('static int track_effects(',start)
            s=s[:start]+'#include "ForceRoot.h"\nstatic void discover_model(void){discover_force_model();}\n'+s[end:]
            s=s.replace('} RloTrackSnapshot;', ' RloForceClipTarget clip_target;int clip_status;uint32_t editor_track;\n} RloTrackSnapshot;')
            s='#include "ClipProtocol.h"\n'+s
            s=s.replace('#include "EditorTrack.h"','#include "EditorTrack.h"\n#include "EditorFollow.h"\n#include "BounceClip.h"').replace(' follow_editor_track();',' force_picker_refresh();\n force_follow_editor_track();')
            s=s.replace(' next->editor_fx=', ''' next->editor_track=visible_editor_fx?last_editor_owner.track:0;
  next->clip_target=(RloForceClipTarget){0};
  next->clip_status=RLO_CLIP_UNAVAILABLE;
  if(valid&&count&&force_bounce_fingerprint()){
   uint32_t engine=force_engine();
   if(engine){
    next->clip_target.project=(uint32_t)native_project;next->clip_target.track=track;
    next->clip_status=force_clip_target(track,&next->clip_target)?RLO_CLIP_BOUNCED:RLO_CLIP_SELECTION_CHANGED;
   }
  }
  next->editor_fx=''')
            s=s.replace(' if(requested&&published)', ' force_bounce_refresh(track,fx,valid?count:0);\n if(requested&&published)')
        if path.name=='EditorTrack.h':
            s,n=re.subn(r'app=native_word\(native_base\+0x[0-9a-f]+\);','app=(uint32_t)native_application;',s);assert n==1
        if path.name=='PadBridge.h':
            s=s.replace(' rlo_pad_input(&pad_parser,bytes,size,pad_input_sink,0);',
                '#ifdef RLO_PAD_BRIDGE_TEST\n rlo_pad_input(&pad_parser,bytes,size,pad_input_sink,0,0);\n#else\n rlo_pad_input(&pad_parser,bytes,size,pad_input_sink,0,force_picker_pad);\n#endif')
        if path.name=='ButtonBridge.c':
            before='((d&&(strstr(d,"MPC")||strstr(d,"Force")))||strstr(s,"MPC")||strstr(s,"Force"))'
            assert before in s;s=s.replace(before,'((d&&strstr(d,"Force"))||strstr(s,"Force"))')
        if path.name=='FakeAlsa.c':s=s.replace('MPC One +','Akai Pro Force').replace('MPC Studio Live Private','Akai Pro Force Private')
        if path.name=='BridgePassThroughTest.c':s=s.replace('0x90,73','0xb0,73').replace('0x90,80','0xb0,80')
        if path.name=='exports.map':
            s=re.sub(r'^.*retrolooper_force_timeline_.*\n','',s,flags=re.M)
            s=s.replace('local:', ''.join('    '+symbol+';\n' for symbol in clip_exports)+'  local:')
        if path.name=='check_exports.py':
            s=s.replace('retrolooper_buttons.so','retrolooper_force_controls.so')
        s=s.replace('/tmp/retrolooper-track-state.json','/tmp/retrolooper-force-track-state.json').replace('/tmp/retrolooper-pad-state.json','/tmp/retrolooper-force-pad-state.json')
        s=s.replace('MPC One+ 3.10.0.49','Force 3.10').replace('Pinned MPC 3.10.0.49','Pinned Force 3.10')
        path.write_text(s)
    for name in ['ForceControlTest.c','EditorFollowModelTest.c','addin.manifest','README.txt']:shutil.copy2(ROOT/'control'/name,dest/name)
    for name in ['TrackBridgeModelTest.c','EditorTrackModelTest.c','BridgePassThroughTest.c','ForceControlTest.c']:
        path=dest/name;path.write_text('#ifdef NDEBUG\n#error Force isolated tests require active assertions\n#endif\n'+path.read_text())
    (dest/'firmware-profile.json').write_text(json.dumps(profile,indent=2)+'\n')
    return dest
if __name__=='__main__':generate(ROOT/'workspace')
