# Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
# SPDX-License-Identifier: GPL-3.0-only
"""Parameter-driven native artwork using the supplied Akai sprite sheet.
Static textured cassette + small animated hub strips keep the texture budget low.
The waveform, meters, hold and clip retain their existing engine parameters.
"""
import json
import math
from pathlib import Path
from PIL import Image, ImageDraw
from hardware_ui import font, BG, WELL, TEXT, MUTED, LINE, ACCENT, panel
from parameters import PARAMS
ROOT=Path(__file__).resolve().parents[1]
BOX=(0,164,1280,300)
INDEX={p['key']:i for i,p in enumerate(PARAMS)}
WARN='#ffcc43';CLIP='#ff454e';SAFE='#55ee76'

def mosaic(w,h,t,hold=False,safe=SAFE):
    im=Image.new('RGBA',(w,h),(0,0,0,0) if hold else WELL);d=ImageDraw.Draw(im);rows=h//6
    active=int(t*rows);peak=min(rows-1,math.ceil(t*rows)-1) if t>.02 else -1
    for row in range(rows):
        if hold and row!=peak:continue
        c=CLIP if row/rows>.85 else WARN if row/rows>.65 else safe
        rgb=tuple(int(c[i:i+2],16) for i in (1,3,5));bg=tuple(int(WELL[i:i+2],16) for i in (1,3,5));a=.95 if hold or row<active else .13
        ink=tuple(round(x*a+y*(1-a)) for x,y in zip(rgb,bg))+(255,)
        y=h-(row+1)*6
        for x in range(0,w,6):d.rectangle((x,y,min(x+4,w-1),y+3),fill=ink)
    return im

def cassette_pack(fill,takeup=False):
    """Smooth reel face without drawn tape layers or tape lines."""
    scale=3;im=Image.new('RGBA',(120*scale,138*scale));d=ImageDraw.Draw(im)
    cx,cy=60*scale,58*scale;amount=fill if takeup else 1-fill
    radius=math.sqrt(25**2+(50**2-25**2)*amount)*scale
    d.ellipse((cx-radius,cy-radius,cx+radius,cy+radius),fill='#202b32',outline='#566873',width=scale)
    d.ellipse((cx-26*scale,cy-26*scale,cx+26*scale,cy+26*scale),fill='#0b121a',outline='#77796e',width=scale)
    return im.resize((120,138),Image.Resampling.LANCZOS)

def cassette_hub(phase):
    """One circular six-slot plastic hub, seen from the front of the cassette."""
    scale=4;im=Image.new('RGBA',(56*scale,56*scale));d=ImageDraw.Draw(im);c=28*scale
    def disk(r,fill,outline=None):
        rr=r*scale;d.ellipse((c-rr,c-rr,c+rr,c+rr),fill=fill,outline=outline,width=scale)
    disk(25,'#686f68','#b2b5a3');disk(22,'#d6d7bc','#e9e7cf')
    # Negative screen angle carries tape left-to-right along the lower path.
    for i in range(6):
        a=-phase*math.tau+i*math.tau/6
        points=[]
        for r,theta in ((10,a-.17),(20,a-.22),(20,a+.22),(10,a+.17)):
            points.append((c+r*scale*math.cos(theta),c+r*scale*math.sin(theta)))
        d.polygon(points,fill='#17242a')
    disk(8,'#53615b');disk(5,'#050c12')
    a=-phase*math.tau;xx=c+23*scale*math.cos(a);yy=c+23*scale*math.sin(a)
    d.ellipse((xx-scale,yy-scale,xx+scale,yy+scale),fill='#ffffff')
    return im.resize((56,56),Image.Resampling.LANCZOS)

def make(skin,S):
    skin=Path(skin);tui=json.loads((skin/'TUI.json').read_text());definitions=tui['pageData']['componentDefinitions']['localComponentDefinitions'];page=tui['pageData']['tabs'][0]['componentDefinition']['componentsData'];original=page[1:];del page[1:]
    manifest={'box':list(BOX),'views':['Waveform','Cassette','Meters'],'drawables':[]};created=set()
    def conditions(c,mode,condition=None):
        handles=[] if mode==-1 else [f'IndexedEnabling/{mode}/3/Parameter {INDEX["view"]}']
        if condition:handles.append(f'IndexedEnabling/{condition[1]}/3/Parameter {INDEX[condition[0]]}')
        c['bounds']['additionalInvalidatingHandles']=handles;return c
    def image(name,im,x,y,mode):
        im.save(skin/name);page.append(conditions(S._sub('Image',{'version':2,'imageType':'Regular','colour':'0','image':name},S._bounds(x,y-86,*im.size),name),mode));manifest['drawables'].append(dict(image=name,x=x,y=y,w=im.width,h=im.height,view=mode))
    def strip(name,w,h,frames,paint):
        if h*frames>12288:raise ValueError('Filmstrip exceeds texture budget')
        atlas=Image.new('RGBA',(w,h*frames))
        for i in range(frames):atlas.paste(paint(i/(frames-1)),(0,i*h))
        atlas.save(skin/name)
    def widget(name,param,x,y,w,h,frames,mode,condition=None):
        key='rloVisual_'+name.replace('.png','')
        if key not in created:
            definition=S._local(key,[],[S._sub('Knob',{'version':1,'knobType':'FilmStrip','filmStrip':name,'numFrames':frames,'handleName':'Data'},S._bounds(0,0,w,h),'Display')]);definition['value']['ignoreMousePresses']=True;definitions.append(definition);created.add(key)
        page.append(conditions(S._placed(key,param,INDEX[param],x,y,w,h,focus='No'),mode,condition))
        item=dict(image=name,x=x,y=y,w=w,h=h,frames=frames,param=param,view=mode)
        if condition:item['condition']=dict(param=condition[0],value=condition[1],max=2)
        manifest['drawables'].append(item)
    def bar(w,h,t,upper,color=ACCENT):
        im=Image.new('RGBA',(w,h));d=ImageDraw.Draw(im);n=round(t*(h-2))
        if n:
            d.rectangle((0,h-n if upper else 0,w-1,h-1 if upper else n-1),fill=color)
            if w>3:d.line((w//2,h-n if upper else 0,w//2,h-1 if upper else n-1),fill='#b2f1ff',width=1)
        return im
    def scope_background(im,rect):
        d=ImageDraw.Draw(im);x,y,w,h=rect;d.rectangle((x,y,x+w-1,y+h-1),fill=WELL)
        for xx in range(x,x+w,12):d.line((xx,y,xx,y+h-1),fill='#0b1c2a')
        for yy in range(y,y+h,12):d.line((x,yy,x+w-1,yy),fill='#0b1c2a')
        d.line((x,y+h//2,x+w-1,y+h//2),fill='#285268')
    def scope(prefix,x,y,w,h,mode):
        step=w//48;barw=step-1;w=step*48;half=h//2
        strip(prefix+'_hi.png',barw,half,33,lambda t:bar(barw,half,t,True))
        strip(prefix+'_lo.png',barw,half,33,lambda t:bar(barw,half,t,False))
        for i in range(48):
            widget(prefix+'_hi.png',f'wave_hi_{i:02}',x+i*step,y,barw,half,33,mode)
            widget(prefix+'_lo.png',f'wave_lo_{i:02}',x+i*step,y+half,barw,half,33,mode)
        # Reuse a short cursor tile down the scope: 192 horizontal positions
        # without an oversized full-height filmstrip. Every tile shares one
        # texture and parameter, so the vertical line stays continuous.
        cursor_head(prefix+'_head.png',x,y,w,h,mode,WARN)
    def cursor_head(name,x,y,w,h,mode,color):
        parts=4 if h==140 else 3;tile=h//parts;frames=192
        def paint(t):
            im=Image.new('RGBA',(w,tile));ImageDraw.Draw(im).line((round(t*(w-1)),0,round(t*(w-1)),tile-1),fill=color,width=2);return im
        strip(name,w,tile,frames,paint)
        for i in range(parts):widget(name,'position',x,y+i*tile,w,tile,frames,mode)
    # Side meters are visible in every display mode, using the existing stereo data.
    strip('rlo_side_peak.png',18,210,49,lambda t:mosaic(18,210,t))
    strip('rlo_side_rms.png',3,210,49,lambda t:bar(3,210,t,True,ACCENT))
    for path,ch,x in (('input','l',30),('input','r',56),('output','l',1204),('output','r',1230)):
        widget('rlo_side_peak.png',f'{path}_peak_{ch}',x,196,18,210,49,-1)
        widget('rlo_side_rms.png',f'{path}_rms_{ch}',x+7,196,3,210,49,-1)
    for mode in range(3):
        bg=Image.new('RGBA',(1080,298),BG);d=ImageDraw.Draw(bg)
        if mode==1:
            # The cream/rainbow cassette is an actual crop of the user's sprite art.
            cassette=Image.open(ROOT/'art/reference/akai-concept.jpeg').convert('RGBA').crop((54,49,490,181)).resize((984,298),Image.Resampling.LANCZOS)
            bg.alpha_composite(cassette,(48,0))
            # Remove the baked reel/scope image and fit real circular mechanisms
            # into the original long cassette window.
            d.rounded_rectangle((178,58,900,212),radius=76,fill='#34424c',outline='#63717c',width=2)
            d.rounded_rectangle((184,64,894,206),radius=70,fill=WELL,outline='#111e2b',width=3)
            for cx in (242,836):
                d.ellipse((cx-58,72,cx+58,188),fill='#0c1720',outline='#53616c',width=2)
            scope_background(bg,(320,82,432,96))
        else:
            panel(d,(48,0,1032,296))
            d.rounded_rectangle((64,16,1016,52),radius=4,fill='#e9dfc0')
            d.text((84,34),'RETROLOOPER / '+('WAVEFORM' if mode==0 else 'LEVEL METER'),font=font(24),fill='#19222a',anchor='lm')
            for j,c in enumerate(('#ff454e','#ff943a','#ffcf45','#38b7ce','#1876a6')):d.rectangle((784,19+j*6,998,24+j*6),fill=c)
            if mode==0:scope_background(bg,(72,68,936,168))
            else:
                d.text((78,78),'LOOP LEVEL / PEAK HOLD',font=font(22),fill=MUTED)
                for db in (0,-12,-24,-36,-48):d.text((740,104+round(-db/48*124)),str(db),font=font(20),fill=MUTED,anchor='lm')
                d.text((850,82),'IN',font=font(22),fill=MUTED,anchor='mm');d.text((954,82),'OUT',font=font(22),fill=MUTED,anchor='mm')
        # Crisp, real-time state and values replace any baked screenshot captions.
        d.rounded_rectangle((48,250,1032,286),radius=4,fill=WELL,outline='#2b4659')
        image(f'rlo_view_{mode}.png',bg,100,164,mode)
        if mode==0:scope('rlo_wave',172,232,936,168,0)
        elif mode==1:
            scope('rlo_tape_wave',420,246,432,96,1)
            # Keep the reference cassette's A face; crossfader assignment is retired.
            # Supply and take-up hubs use separate phases at v/r. Their tape
            # packs share one fill parameter, so one shrinks as the other grows.
            for side,x,cx,param in (('supply',282,342,'reel_phase'),('takeup',876,936,'takeup_phase')):
                name=f'rlo_pack_{side}.png';strip(name,120,138,33,lambda t,side=side:cassette_pack(t,side=='takeup'))
                widget(name,'tape_fill',x,236,120,138,33,1)
                name='rlo_hub.png'
                if side=='supply':strip(name,56,56,192,cassette_hub)
                widget(name,param,cx-28,266,56,56,192,1)
        else:
            strip('rlo_focus.png',644,140,49,lambda t:mosaic(644,140,t));widget('rlo_focus.png','focus_meter',178,266,644,140,49,2)
            strip('rlo_hold.png',644,140,49,lambda t:mosaic(644,140,t,True));widget('rlo_hold.png','focus_hold',178,266,644,140,49,2)
            strip('rlo_clip.png',644,4,2,lambda t:Image.new('RGBA',(644,4),CLIP if t else (0,0,0,0)));widget('rlo_clip.png','focus_clip',178,266,644,4,2,2)
            cursor_head('rlo_meter_head.png',178,266,644,140,2,'#ffffff99')
            strip('rlo_peak.png',24,140,49,lambda t:mosaic(24,140,t))
            for path,ch,x in (('input','l',924),('input','r',958),('output','l',1028),('output','r',1062)):
                widget('rlo_peak.png',f'{path}_peak_{ch}',x,266,24,140,49,2)
    page.extend(original)
    S.to_mpc2x(tui);(skin/'TUI.json').write_text(json.dumps(tui,indent=2)+'\n');(ROOT/'preview/visuals.json').write_text(json.dumps(manifest,indent=2)+'\n');return manifest
