"""Build and preview the production hardware UI over the generated Q-Link map."""
import argparse
from pathlib import Path
import sys
import json
from PIL import Image

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--kit',required=True);a=p.parse_args()
sys.path.insert(0,str(Path(a.kit)/'tools'))
import shadow_skin
import hardware_ui
import visual_skin
import layout_audit

skin=ROOT/'build/skin/RetroLooper - VST - Retrolooper One/Plugin Skins'
(ROOT/'preview').mkdir(exist_ok=True)
manifest=hardware_ui.build(skin,shadow_skin)
visual_skin.make(skin,shadow_skin)
print('Native touch controls:',hardware_ui.native_touch_controls(skin))
hardware_ui.preview(skin,shadow_skin,manifest)
layout_audit.main()
references=set()
def collect(node):
    if isinstance(node,dict):
        for value in node.values():collect(value)
    elif isinstance(node,list):
        for value in node:collect(value)
    elif isinstance(node,str) and node.lower().endswith('.png'):references.add(Path(node).name)
collect(json.loads((skin/'TUI.json').read_text()))
removed=0
for image in skin.glob('*.png'):
    if image.name not in references:image.unlink();removed+=1
sizes=[]
for name in references:
    with Image.open(skin/name) as img:sizes.append(img.size)
pixels=sum(w*h for w,h in sizes)
(ROOT/'preview/resource-audit.json').write_text(json.dumps(dict(referenced_pngs=len(references),removed_unused_pngs=removed,
    decoded_rgba_mib_if_all_cached=round(pixels*4/1024**2,1),knob_frames=hardware_ui.KNOB_FRAMES,maximum_image_height=max(h for w,h in sizes),
    visual_strip_height_limit=12288),indent=2)+'\n')
print('Hardware UI: five pages, shared native/browser geometry, 56px minimum targets, 88px primary transport.')
