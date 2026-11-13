#!/usr/bin/env python3
"""Isolated Force core/UI build. Never installs the MPC-only native helper.
Generated sources, engine snapshot and skin live below force/workspace.
"""
import argparse,hashlib,importlib.util,json,os,re,shutil,subprocess,sys
from pathlib import Path
import record_offset
ROOT=Path(__file__).resolve().parent
SHARED=ROOT.parent
WORK=ROOT/'workspace'
PROFILE=json.loads((ROOT/'profile.json').read_text())
SKIN_FOLDER=f"{PROFILE['vendor']} - VST - {PROFILE['name']}"
CHANGES=[]
def adapt(path,before,after,count=None):
    s=path.read_text();n=s.count(before)
    if not n or count is not None and n!=count:raise RuntimeError(f'Review Force adaptation in {path.name}: {before!r}, found {n}')
    path.write_text(s.replace(before,after));CHANGES.append(dict(file=str(path.relative_to(WORK)),before=before,after=after,count=n))
def run(args,**kw):
    print('+',' '.join(map(str,args)),flush=True);subprocess.run(list(map(str,args)),check=True,**kw)
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
def prepare():
    WORK.mkdir(parents=True,exist_ok=True)
    for directory in ['src','tools','art','tests']:
        shutil.copytree(SHARED/directory,WORK/directory,dirs_exist_ok=True,ignore=shutil.ignore_patterns('__pycache__'))
    (WORK/'control').mkdir(exist_ok=True)
    shutil.copy2(SHARED/'control/TrackProtocol.h',WORK/'control/TrackProtocol.h')
    for file in ['layout.conf','params.lock']:shutil.copy2(SHARED/file,WORK/file)
    cfg=json.loads((SHARED/'vst.json').read_text());cfg.update({k:PROFILE[k] for k in ['name','vendor','uid','version','so','folder_name']})
    (WORK/'vst.json').write_text(json.dumps(cfg,indent=2)+'\n')
    source=WORK/'src/Plugin.cpp'
    record_offset.prepare(WORK,adapt)
    old=json.loads((SHARED/'vst.json').read_text())
    adapt(source,f'kVersion={old["version"]},kUid=0x526c4f31',f'kVersion={PROFILE["version"]},kUid=0x526c4631',1)
    adapt(source,'"'+old['name']+'"','"'+PROFILE['name']+'"',1)
    adapt(source,'retrolooper_mpc_',PROFILE['bridge_prefix'],9)
    # Keep /proc/self/exe's actual basename "MPC" intact: Force also runs MPC.
    translations={'WAIT MPC PLAY':'WAIT FORCE PLAY','WAITING FOR MPC CLOCK':'WAITING FOR FORCE CLOCK',
        'MPC LENGTH UNAVAILABLE':'HOST LENGTH UNAVAILABLE','ARMED: MPC SEQ START':'ARMED: HOST LOOP START',
        'PREPARING MPC TRACK':'PREPARING FORCE TRACK','WAIT MPC TRACK BRIDGE':'FORCE SETUP REQUIRED',
        'WAIT MPC LENGTH':'WAIT FORCE LENGTH','WAIT MPC':'WAIT FORCE','MPC REC':'FORCE REC','MPC PLAY':'FORCE PLAY',
        'MPC STOP':'FORCE STOP','MPC CLICK':'FORCE CLICK','PAD BANK A OFF':'LOOPER PADS OFF',
        'PRESS PAD BANK A':'SELECT LOOPER PADS','PAD A / LED WAIT':'PADS / LED WAIT','PAD BANK A READY':'LOOPER PADS READY',
        'SEQ %.2g > %.2g B':'LOOP %.2g > %.2g B','A%d%s':'P%d%s'}
    for before,after in translations.items():adapt(source,'"'+before+'"','"'+after+'"')
    for file in ['src/TestBridge.cpp','tests/HostTests.cpp']:
        path=WORK/file;adapt(path,'retrolooper_mpc_',PROFILE['bridge_prefix'])
        s=path.read_text()
        for before,after in translations.items():s=s.replace('"'+before+'"','"'+after+'"')
        if file.startswith('tests/'):
            s='#ifdef NDEBUG\n#error Force host tests require active assertions\n#endif\n'+s
            s=s.replace('=="A4+"','=="P4+"').replace('"SEQ ','"LOOP ').replace('RetroLooper One native ABI','Retrolooper One Force native ABI')
            # The ARM diagnostic avoids libc++ console-stream interposition;
            # keep every assertion and the DSP path unchanged.
            s=re.sub(r'std::cout<<("(?:\\.|[^"\\])*");',r'std::fputs(\1,stdout);std::fflush(stdout);',s)
        path.write_text(s)
    p=WORK/'tools/parameters.py'
    adapt(p,'PAD_DEFAULTS=', '''_bounce=[dict(key=key,name=name,min=0,max=1,default=0,kind="trigger",momentary=True)
         for key,name in [("bounce_clip","Choose Clip"),("bounce_commit","Bounce to Clip"),("bounce_cancel","Cancel Bounce")]]
_offset=dict(key="record_offset",name="Record Offset",min=-1,max=50,default=-1,kind="integer")
MIDI_TARGETS.extend(_bounce+[_offset])
VISIBLE_MIDI_TARGETS.extend(_bounce+[_offset])
PAD_DEFAULTS=''',1)
    adapt(p,'add("pad_edit_mode","Pad Page Mode",options=["Play","Assign"])',
        'add("pad_edit_mode","Pad Page Mode",options=["Play","Assign"])\nPARAMS.extend(_bounce)\nadd("clip_picker","Clip Picker",kind="readout")\nPARAMS.append(_offset)\nadd("clip_indicator","Clip Indicator",kind="readout")',1)
    extra='''# Force display metadata; released parameter keys/order and defaults stay identical.
for _p in PARAMS:
    _p['name']=_p['name'].replace('MPC ', 'Force ').replace('Pad Bank A','Looper Pads').replace('Pad A','Pad P')
    if _p['key']=='capture_source': _p['options']=['Force Rec','Touch/Pads']
    if _p['key'] in ('length_ratio','sequence_info'): _p['name']='Host Loop Length'
    if _p['key']=='launch_mode': _p['options']=['Arrange','Next bar']
_pad_labels={'Play / Stop':'Play','Stop Now':'Stop now','Sync to Start':'Sync start','Global Timing':'Timing','Input Gain':'Input gain','Loop Volume':'Volume','Record Input':'Input','Loop Output':'Output','Dub Feedback':'Feedback','Record Timing':'Rec timing','Overdub Timing':'Dub timing','After Record':'After rec','Target Bars':'Take bars','Half Length':'Half len','Double Length':'Double len','Restore One Bar':'Restore bar','Loop High Pass':'High pass','Loop Low Pass':'Low pass','Start Threshold':'Threshold','Visual View':'View','Host Loop Length':'Length','Sequence Length':'Length','Loop Launch':'Launch','MIDI Learn':'Learn','Previous MIDI Target':'Prev MIDI','Next MIDI Target':'Next MIDI','Forget Binding':'Forget','Learn Target':'MIDI target','Learn Gesture':'Gesture','Capture Controls':'Controls','Record Mode':'Rec mode','Mute Mode':'Mute mode','Stop Timing':'Stop time','Play Timing':'Play time','Mute Timing':'Mute time','Trim One Bar':'Trim bar','Set Bars':'Set bars','WAV Slot':'WAV slot','Loop Fade':'Loop fade','Capture Control':'Capture','Looper Pads':'Pads'}
for _p in PARAMS:
    if _p.get('pad_assignment'): _p['options']=[_pad_labels.get(_v,_v) for _v in _p['options']]
for _p in PARAMS:
    if _p['key']=='learn_target': _p['options']=[_t['name'] for _t in VISIBLE_MIDI_TARGETS]

'''
    adapt(p,'def generate():',extra+'def generate():',1)
    adapt(p,'name="Retrolooper One"','name="'+PROFILE['name']+'"',1)
    adapt(WORK/'layout.conf','PAD A','PAD P')
    for before,after in [('Sequence / take bars','Arrange / take bars'),('Sequence length','Arrange length')]:adapt(WORK/'layout.conf',before,after)
    layout=WORK/'layout.conf'
    adapt(layout,'label="EXPORT" key=export_audio','label="WAV" key=export_audio\nbutton cx=380 cy=670 w=120 h=56 label="CANCEL" key=bounce_cancel',1)
    adapt(layout,'button cx=508 cy=638 w=120 h=120 label="REVERSE" key=reverse',
        'button cx=508 cy=606 w=120 h=56 label="REVERSE" key=reverse\nbutton cx=508 cy=670 w=120 h=56 label="CLIP" key=bounce_clip\nbutton cx=508 cy=670 w=120 h=56 label="BOUNCE" key=bounce_commit',1)
    adapt(layout,'knob cx=280 cy=640 r=28 bw=344','knob cx=156 cy=640 r=28 bw=248',1)
    adapt(layout,'button cx=720 cy=640 w=280','button cx=426 cy=640 w=230',1)
    adapt(layout,'button cx=1098 cy=640 w=280 h=64 label="Export WAV" key=export_audio',
        'button cx=700 cy=640 w=230 h=64 label="Export WAV" key=export_audio\nbutton cx=700 cy=640 w=230 h=64 label="Cancel Bounce" key=bounce_cancel\nbutton cx=1010 cy=640 w=282 h=64 label="Choose Clip" key=bounce_clip\nbutton cx=1010 cy=640 w=282 h=64 label="Bounce to Clip" key=bounce_commit\nreadout cx=640 cy=576 w=960 h=28 label="" key=message',1)
    adapt(layout,'file_slot,import_audio,export_audio,trim,restore,bars,set_bars,reverse',
        'file_slot,import_audio,export_audio,bounce_clip,trim,restore,bars,set_bars',1)
    adapt(layout,'button cx=952 cy=667 w=568 h=56 label="Align loop to start" key=sync_start',
        'button cx=796 cy=667 w=244 h=56 label="Align to start" key=sync_start\nknob cx=1101 cy=667 r=22 bw=318 bh=56 lay=side label="Record offset" key=record_offset',1)
    adapt(layout,'mute_timing,after_record,sync_start','mute_timing,after_record,sync_start,record_offset',1)
    s=layout.read_text();start=s.index('[tab PADS]');end=s.index('[tab MIDI]',start)
    rows=['[tab PADS]','readout cx=554 cy=120 w=352 h=32 label="" key=pad_status','enum_h cx=872 cy=120 label="" key=pad_edit_mode sw=112 sh=56','enum_h cx=1130 cy=120 label="" key=pad_bank_enabled sw=106 sh=56']
    for top,pads in enumerate([range(9,17),range(1,9)]):
        for col,pad in enumerate(pads):
            center=f'cx={94+156*col} cy={312+222*top}'
            rows.append(f'button {center} w=136 h=136 label="P{pad}" key=pad_press_{pad:02}')
            rows.append(f'knob {center} r=34 bw=136 label="P{pad}" key=pad_a_{pad:02}')
    rows.append('qlinks "PADS" = '+','.join(f'pad_a_{pad:02}' for pad in range(1,17)));layout.write_text(s[:start]+'\n'.join(rows)+'\n'+s[end:])
    ui=WORK/'tools/hardware_ui.py'
    adapt(ui,'BUTTON_FEEDBACK={','BUTTON_FEEDBACK={"bounce_clip":"clip_indicator",',1)
    adapt(ui,'Parameter {INDEX["pad_edit_mode"]}', 'Parameter {INDEX[condition["param"]]}',1)
    adapt(ui,";p=PARAMS[INDEX[param]];cx=", """
            if tab['name'] in ('LOOPER','MIXER') and param in ('export_audio','bounce_clip','bounce_commit','bounce_cancel'):
                condition=dict(param='clip_picker',value=int(param in ('bounce_commit','bounce_cancel')),max=1)
            p=PARAMS[INDEX[param]];cx=""",1)
    adapt(ui,'press a Bank A pad or send a MIDI note / CC.','send an assigned pad note or MIDI CC.',1)
    for before,after in [('SEQ 4 > 4 B','LOOP 4 > 4 B'),('MPC STOP','FORCE STOP'),('PAD BANK A READY','LOOPER PADS READY')]:adapt(ui,before,after)
    adapt(ui,"f'A{PAD_DEFAULTS.index(action)+1}'","f'P{PAD_DEFAULTS.index(action)+1}'",1)
    adapt(WORK/'tools/preview_server.py','Retrolooper One native testbench','Retrolooper One · Force testbench',1)
    page=WORK/'tools/preview.html';s=page.read_text().replace('Retrolooper One',PROFILE['name']).replace('Force / MPC One+','Force core / hardware integration pending').replace('MPC','Force').replace('Bank A','Looper').replace('Sequence','Host loop')
    s=s.replace('<p class="foot">','<p class="foot"><strong>Core preview: Force track/grid integration is pending hardware validation.</strong> ');page.write_text(s)
    # Keep the proven gesture geometry; Force has native version-5 filmstrip knobs.
    adapt(WORK/'tools/skin_finish.py','hardware_ui.build(skin,shadow_skin)','hardware_ui.build(skin,shadow_skin,compact_pads=True)',1)
    (WORK/'provenance.json').write_text(json.dumps(dict(profile=PROFILE,adaptations=CHANGES,
        source_sha256={str(p.relative_to(SHARED)):hashlib.sha256(p.read_bytes()).hexdigest() for directory in ['src','tools','tests'] for p in (SHARED/directory).rglob('*') if p.is_file() and '__pycache__' not in str(p)}),indent=2)+'\n')
    module('force_native_control',ROOT/'native_control.py').generate(WORK)
    run([sys.executable,WORK/'tools/parameters.py'])
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',choices=['prepare','test','test-arm','arm','skin','preview','package','all']);p.add_argument('--kit',default='/tmp/retrolooper-kit031');p.add_argument('--zig',default='/tmp/zig-macos-aarch64-0.13.0/zig');p.add_argument('--sanitize',action='store_true');a=p.parse_args()
    prepare();base=module('shared_akai_build',SHARED/'build.py');base.ROOT=WORK;base.BUILD=WORK/'build';base.BUILD.mkdir(exist_ok=True);base.snapshot(force_record_offset=True)
    flags=base.compile_flags()+['-DRETROLOOPER_FORCE_BOUNCE'];sources=[base.BUILD/'engine'/s for s in base.ENGINE_SOURCES]+[WORK/'src/Plugin.cpp']
    env={**os.environ,'ZIG_GLOBAL_CACHE_DIR':str(base.BUILD/'zig-cache'),'ZIG_LOCAL_CACHE_DIR':str(base.BUILD/'zig-local')}
    if a.action in ['test','all']:
        checked=flags+['-DRETROLOOPER_TESTING']+(['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g'] if a.sanitize else [])
        run(['clang++',*checked,*sources,WORK/'tests/HostTests.cpp','-Wl,-export_dynamic','-o',base.BUILD/'host-tests'])
        run([base.BUILD/'host-tests'],env={**os.environ,'RETROLOOPER_TEST_LOOPS':str(base.BUILD/'test-loops')})
    if a.action=='test-arm':
        run([a.zig,'c++','-target',PROFILE['target'],'-mcpu='+PROFILE['cpu'],*flags,'-UNDEBUG','-DRETROLOOPER_TESTING',*sources,WORK/'tests/HostTests.cpp','-Wl,--export-dynamic','-ldl','-o',base.BUILD/'host-tests-armv7'],env=env)
    if a.action in ['arm','all']:
        run([a.zig,'c++','-target',PROFILE['target'],'-mcpu='+PROFILE['cpu'],*flags,'-shared',*sources,'-s','-Wl,--no-undefined','-Wl,--version-script='+str(WORK/'src/exports.map'),'-ldl','-o',base.BUILD/PROFILE['so']],env=env)
    if a.action in ['preview','all']:
        run(['clang++',*flags,'-dynamiclib',*sources,WORK/'src/TestBridge.cpp','-o',base.BUILD/'preview-engine.dylib'])
    if a.action in ['skin','package','all']:
        kit=Path(a.kit).resolve();assert subprocess.check_output(['git','-C',str(kit),'rev-parse','HEAD'],text=True).strip()==base.KIT_COMMIT
        run(['cc','-O2','-I'+str(kit/'tools/vendor/force-shadow/tools'),kit/'tools/shadow_art.c','-lm','-o',base.BUILD/'shadow_art'])
        run([sys.executable,kit/'tools/gen_vst.py',WORK/'vst.json'],cwd=WORK,env={**os.environ,'SHADOW_SKIN_MPC_OS':'2'})
        # Force locates the skin by the VST product title even if the library
        # itself is registered from a different folder.
        skin=base.BUILD/'skin'/SKIN_FOLDER
        run([sys.executable,WORK/'tools/skin_finish.py','--kit',kit])
        run([sys.executable,kit/'tools/skin_check.py',skin]);run([sys.executable,WORK/'tools/check_native_skin.py','--kit',kit])
    if a.action in ['package','all']:
        run([sys.executable,kit/'tools/release.py','--so',base.BUILD/PROFILE['so'],'--skin',skin,'--entry',base.BUILD/'pluginlist-entry.xml','--version',PROFILE['release'],'--id','retrolooper-force','--about','Stereo Audio insert looper with host sync, touch controls and MIDI. Native pads and Bounce to Clip are optional Force Controls features.','--user-data','Loops','--user-data','Exports','--extra',ROOT/'README.md:FORCE-STATUS.md','--extra',ROOT/'profile.json:force-profile.json','--extra',SHARED/'vendor/mpc-vst-plugins-LICENSE:licenses/mpc-vst-plugins-LICENSE','--extra',base.REPO/'third_party/signalsmith-stretch/LICENSE.txt:licenses/signalsmith-stretch-LICENSE.txt','--extra',base.REPO/'third_party/signalsmith-linear/LICENSE.txt:licenses/signalsmith-linear-LICENSE.txt','-o',ROOT/'dist'])
        title_slug=re.sub(r'[^A-Za-z0-9]+','-',PROFILE['name']).strip('-')
        original=ROOT/f'dist/{title_slug}-{PROFILE["release"]}-mpc-armv7.zip'
        archive=ROOT/f'dist/RetroLooper-One-Force-{PROFILE["release"]}-mpc-armv7.zip'
        if original!=archive:original.replace(archive)
        run([sys.executable,kit/'tools/catalog_check.py',archive])
if __name__=='__main__':main()
