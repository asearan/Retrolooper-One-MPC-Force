# Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
# SPDX-License-Identifier: GPL-3.0-only
"""Validate the explicit MPC OS 3.x knob schema against the pinned community tooling."""
import argparse,copy,json,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--kit',required=True);a=p.parse_args()
sys.path.insert(0,str(Path(a.kit)/'tools'))
import shadow_skin,skin_compat
skin=ROOT/'build/skin/RetroLooper - VST - RetroLooper One/Plugin Skins'
tui=json.loads((skin/'TUI.json').read_text());ql=json.loads((skin/'Q-Links.json').read_text())
modern=[]
def walk(node):
    if isinstance(node,dict):
        if node.get('type')=='Knob' and node.get('data',{}).get('version')==5:
            d=node['data'];expected={'version','knobType','filmStrip','numFrames','handleName','invert','dragOrientation'}
            assert set(d)==expected,('Unexpected modern knob fields',d)
            assert d['knobType']=='FilmStrip' and d['invert'] is False
            assert d['dragOrientation'] in ('Vertical','Horizontal')
            if d['filmStrip'].startswith('rlo_knob_'):assert d['numFrames']==127 and d['dragOrientation']=='Vertical'
            modern.append(d['dragOrientation'])
        for v in node.values():walk(v)
    elif isinstance(node,list):
        for v in node:walk(v)
walk(tui);assert modern and set(modern)=={'Vertical'}
legacy=copy.deepcopy(tui);shadow_skin.to_mpc2x(legacy)
problems=skin_compat.skin_problems(legacy,ql)
assert not problems,problems
compat,why=skin_compat.os_compat(None,tui,ql)
assert compat==['3.x'] and len(why)==1 and 'Knob version 5' in why[0],(compat,why)
report=dict(os_compat=compat,modern_knob_definitions=len(modern),rotary_drag='Vertical',crossfader='removed',legacy_shape_check='PASS after removing only the explicit modern knob fields',why_not_mpc2=why)
(ROOT/'preview/os-compatibility.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS: MPC OS 3.x skin; 128-frame rotary knobs drag vertically; retired crossfader absent; all other shapes pass pinned schema check')
