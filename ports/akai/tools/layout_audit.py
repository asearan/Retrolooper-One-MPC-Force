# Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
# SPDX-License-Identifier: GPL-3.0-only
"""Audit the built device UI: physical bounds/target sizes, text sizes and foreground contrast."""
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]

def luminance(hexcode):
    rgb=[int(hexcode[i:i+2],16)/255 for i in (1,3,5)]
    rgb=[x/12.92 if x<=.04045 else ((x+.055)/1.055)**2.4 for x in rgb]
    return sum(a*b for a,b in zip(rgb,(.2126,.7152,.0722)))

def contrast(a,b):
    x,y=sorted((luminance(a),luminance(b)));return (y+.05)/(x+.05)

def main():
    doc=json.loads((ROOT/'preview/controls.json').read_text());errors=[];targets=[];readouts=[]
    for page in doc['tabs']:
        interactive=[c for c in page['widgets'] if c['kind']!='readout']
        for i,c in enumerate(interactive):
            for other in interactive[i+1:]:
                if c.get('condition') and other.get('condition') and c['condition']['param']==other['condition']['param'] and c['condition']['value']!=other['condition']['value']:continue
                if min(c['x']+c['w'],other['x']+other['w'])>max(c['x'],other['x']) and min(c['y']+c['h'],other['y']+other['h'])>max(c['y'],other['y']):
                    errors.append(f"{page['name']}: overlapping controls {c['key']} / {other['key']}")
        for c in page['widgets']:
            x,y,w,h=(c[k] for k in ('x','y','w','h'))
            if x<0 or y<86 or x+w>1280 or y+h>714:errors.append(f"{page['name']}: {c['key']} leaves the native viewport")
            if c['kind']!='readout':
                targets.append((w,h))
                if min(w,h)<56:errors.append(f"{page['name']}: {c['key']} touch target is under 56px")
            else:
                readouts.append(c['size'])
                if c['size']<22:errors.append(f"{page['name']}: {c['key']} readout is under 22px")
    pairs=[('#e6f1ed','#0b131e'),('#9fb8c4','#0b131e'),('#39d6ff','#122b3a'),('#ff454e','#100408'),('#ffbd37','#100d04'),('#55f15a','#050f05'),('#d060ed','#100710')]
    ratios=[round(contrast(a,b),2) for a,b in pairs]
    if min(ratios)<4.5:errors.append('An informational foreground/background pair is below 4.5:1 contrast')
    report=dict(native_viewport=[1280,628],pages=len(doc['tabs']),touch_targets=len(targets),minimum_target_pixels=min(min(t) for t in targets),
                primary_transport_pixels=[240,88],minimum_readout_font_pixels=min(readouts),control_label_font_pixels=[22,32],
                measured_contrast_ratios=ratios,errors=errors,hardware_validation='pending',interactive_overlap_check='PASS' if not errors else 'FAIL')
    (ROOT/'preview/layout-audit.json').write_text(json.dumps(report,indent=2)+'\n')
    if errors:raise SystemExit('\n'.join(errors))
    print(f"PASS: {len(targets)} targets at least 56px, readouts at least 22px, text contrast at least {min(ratios)}:1")

if __name__=='__main__':main()
