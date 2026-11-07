# Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
# SPDX-License-Identifier: GPL-3.0-only
"""Touch-first 1280x628 native skin. One geometry manifest drives native and browser controls."""
import copy
import json
import math
from functools import lru_cache
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
from parameters import PARAMS, VISIBLE_MIDI_TARGETS, PAD_COLOURS, PAD_DEFAULTS, PAD_BADGES

ROOT=Path(__file__).resolve().parents[1]
INDEX={p['key']:i for i,p in enumerate(PARAMS)}
BG='#070c13';PANEL='#0b131e';WELL='#020912';TEXT='#e6f1ed';MUTED='#9fb8c4';LINE='#4c6578';ACCENT='#39d6ff'
KNOB_FRAMES=128
BUTTON_FEEDBACK={'record':'record_indicator','play':'play_indicator','dub':'dub_indicator','mute':'mute_indicator','stop_now':'stop_indicator','clear':'clear_indicator'}

@lru_cache(maxsize=128)
def font(px,bold=False):
    return ImageFont.truetype(str(ROOT/'art/VT323-Regular.ttf'),round(px*1.18))

def label(draw,xy,text,size=24,color=TEXT,anchor='la',bold=False):
    draw.text(xy,text,font=font(size,bold),fill=color,anchor=anchor)

def fit(text,width,start=28,minimum=22,bold=True):
    for px in range(start,minimum-1,-1):
        if font(px,bold).getlength(text)<=width:return px
    raise ValueError(f'Label does not fit at {minimum}px: {text!r} in {width}px')

def panel(draw,box,fill=PANEL):
    x,y,r,b=map(round,box)
    draw.rounded_rectangle((x,y,r,b),radius=7,fill=fill,outline=LINE,width=2)
    draw.rounded_rectangle((x+3,y+3,r-3,b-3),radius=5,outline='#1d2b3b')
    draw.line((x+9,y+3,r-9,y+3),fill='#506070')
    draw.line((x+8,b-3,r-8,b-3),fill='#02050a',width=2)

def brand(d):
    colors=['#ff3d4d','#ff8e37','#ffe158','#38b7ce','#1876a6']
    for i,c in enumerate(colors):
        y=12+i*8;d.polygon(((20,y),(94,y),(89,y+7),(20,y+7)),fill=c)
    label(d,(108,25),'RETROLOOPER',44,TEXT,'lm',True)
    label(d,(111,54),'S I N G L E   T R A C K',18,MUTED,'lm')

# Extract only the colored glyphs from the supplied sprite atlas. All surrounds
# and type are rebuilt at native resolution, avoiding stretched JPEG captions.
@lru_cache(maxsize=16)
def sprite_icon(key):
    boxes={'record':(34,198,58,223),'play':(107,197,131,224),'dub':(176,197,205,224),
           'stop_now':(250,197,275,224),'clear':(323,196,348,223)}
    if key not in boxes:return None
    im=Image.open(ROOT/'art/reference/akai-concept.jpeg').convert('RGBA').crop(boxes[key])
    px=im.load()
    for y in range(im.height):
        for x in range(im.width):
            r,g,b,a=px[x,y];v=max(r,g,b);sat=v-min(r,g,b)
            px[x,y]=(r,g,b,round(max(0,min(1,(v-45)/100))*255) if sat>35 else 0)
    return im

def icon(d,kind,x,y,color,scale=1):
    r=10*scale;stroke=max(2,round(2.5*scale))
    if kind in ('undo','redo'):
        d.arc((x-r,y-r,x+r,y+r),135 if kind=='undo' else 45,420 if kind=='undo' else 330,fill=color,width=stroke)
        sign=-1 if kind=='undo' else 1
        d.polygon([(x+sign*r,y-r*.6),(x+sign*r*.1,y-r),(x+sign*r*.4,y+r*.1)],fill=color)

def button_art(path,w,h,text,key,on=False,boolean=False):
    im=Image.new('RGBA',(w*2,h*2));d=ImageDraw.Draw(im);W=w*2;H=h*2
    colors=PAD_COLOURS
    ink=colors.get(key,ACCENT if on else TEXT);border=colors.get(key,ACCENT) if on else LINE
    primary=h>=84 and key in ('record','play','dub','stop_now','clear')
    rgb=tuple(int(ink[i:i+2],16) for i in (1,3,5))
    fill=tuple(round(c*(.22 if on else .055)) for c in rgb)+(255,)
    d.rounded_rectangle((1,1,W-2,H-2),radius=10,fill='#03080f',outline='#25384c',width=2)
    d.rounded_rectangle((6,6,W-7,H-7),radius=7,fill=fill,outline=border,width=3 if not primary else 4)
    d.rounded_rectangle((11,11,W-12,H-12),radius=5,outline=tuple(round(c*.35) for c in rgb)+(255,),width=2)
    if primary:
        sprite=sprite_icon(key).resize((64,64),Image.Resampling.LANCZOS)
        if not on:sprite.putalpha(sprite.getchannel('A').point(lambda a:round(a*.38)))
        im.alpha_composite(sprite,(W//2-32,21))
        label(d,(W/2,H-38),text,26*2,ink,'mm',True)
    elif key=='reverse' and h>=112:
        for tip in (W/2-36,W/2+20):
            d.polygon(((tip-18,75),(tip+20,51),(tip+20,99)),fill=ink)
        size=fit(text,w-22,25,22)
        label(d,(W/2,H-46),text,size*2,ink,'mm',True)
    else:
        size=fit(text,w-(30 if boolean else 22),25,22)
        offset=6 if boolean else 0
        label(d,(W/2+offset*2,H/2+(16 if key in PAD_BADGES else 0)),text,size*2,ink,'mm',True)
        if boolean:
            d.ellipse((16,H/2-5,26,H/2+5),fill=ACCENT if on else '#405264')
    im.resize((w,h),Image.Resampling.LANCZOS).save(path)

def segment_art(path,w,h,text,on,accent=ACCENT):
    im=Image.new('RGBA',(w*2,h*2));d=ImageDraw.Draw(im)
    d.rounded_rectangle((1,1,w*2-2,h*2-2),radius=8,fill='#122b3a' if on else '#080f19',outline=accent if on else LINE,width=3)
    if on:d.line((12,h*2-8,w*2-12,h*2-8),fill=accent,width=3)
    text={'Waveform':'WAVE','Cassette':'TAPE','Meters':'METER'}.get(text,text)
    size=fit(text,w-20,26,22)
    label(d,(w,h),text,size*2,accent if on else TEXT,'mm',True)
    im.resize((w,h),Image.Resampling.LANCZOS).save(path)

def knob_art(path,r):
    s=r*2+10;atlas=Image.new('RGBA',(s,s*KNOB_FRAMES))
    for i in range(KNOB_FRAMES):
        im=Image.new('RGBA',(s*3,s*3));d=ImageDraw.Draw(im);cx=cy=s*1.5;rr=r*3
        d.arc((cx-rr,cy-rr,cx+rr,cy+rr),135,405,fill='#273e50',width=5)
        if i:d.arc((cx-rr,cy-rr,cx+rr,cy+rr),135,135+270*i/(KNOB_FRAMES-1),fill=ACCENT,width=5)
        for inset,c in ((9,'#324457'),(13,'#080e18'),(18,'#253547'),(22,'#111e2e')):
            d.ellipse((cx-rr+inset,cy-rr+inset,cx+rr-inset,cy+rr-inset),fill=c)
        a=math.radians(135+270*i/(KNOB_FRAMES-1));near=rr*.31;far=rr*.73
        d.line((cx+math.cos(a)*near,cy+math.sin(a)*near,cx+math.cos(a)*far,cy+math.sin(a)*far),fill=TEXT,width=5)
        atlas.alpha_composite(im.resize((s,s),Image.Resampling.LANCZOS),(0,i*s))
    atlas.save(path)

def pad_knob_art(path,r):
    s=r*2+10;colours=['#405264']+[PAD_COLOURS[p['key']] for p in VISIBLE_MIDI_TARGETS]
    atlas=Image.new('RGBA',(s,s*len(colours)))
    for frame,colour in enumerate(colours):
        cell=Image.new('RGBA',(s*3,s*3));d=ImageDraw.Draw(cell);cx=cy=s*1.5;rr=r*3
        d.ellipse((cx-rr,cy-rr,cx+rr,cy+rr),fill='#0a111c',outline=colour,width=6)
        d.ellipse((cx-rr+12,cy-rr+12,cx+rr-12,cy+rr-12),fill='#142334',outline='#405264',width=2)
        a=math.radians(135+270*frame/(len(colours)-1));d.line((cx+math.cos(a)*rr*.3,cy+math.sin(a)*rr*.3,cx+math.cos(a)*rr*.72,cy+math.sin(a)*rr*.72),fill=colour,width=6)
        atlas.alpha_composite(cell.resize((s,s),Image.Resampling.LANCZOS),(0,frame*s))
    atlas.save(path)

def pad_button_art(path,w,h):
    colours=['#405264']+[PAD_COLOURS[p['key']] for p in VISIBLE_MIDI_TARGETS]
    atlas=Image.new('RGBA',(w,h*len(colours)*2))
    for index,colour in enumerate(colours):
        rgb=tuple(int(colour[i:i+2],16) for i in (1,3,5))
        for lit in range(2):
            cell=Image.new('RGBA',(w*2,h*2));d=ImageDraw.Draw(cell)
            fill=tuple(round(v*(.26 if lit else .045)) for v in rgb)+(255,)
            line=colour if lit else tuple(round(v*.42) for v in rgb)+(255,)
            d.rounded_rectangle((2,2,w*2-3,h*2-3),radius=12,fill=fill,outline=line,width=4 if lit else 3)
            d.line((18,10,w*2-18,10),fill=colour,width=5 if lit else 2)
            atlas.alpha_composite(cell.resize((w,h),Image.Resampling.LANCZOS),(0,(index*2+lit)*h))
    atlas.save(path)

def build(skin,S):
    skin=Path(skin);tui=json.loads((skin/'TUI.json').read_text());pd=tui['pageData']
    tabs,_=S.parse_layout(str(ROOT/'layout.conf'))
    pd['componentDefinitions']['localComponentDefinitions']=[];definitions=pd['componentDefinitions']['localComponentDefinitions']
    manifest={'width':1280,'height':628,'tabs':[],'assets':[]}
    definitions_seen=set();assets=set()
    def local(key,actions,children):
        if key not in definitions_seen:definitions.append(S._local(key,actions,children));definitions_seen.add(key)
    def place(page,key,param,x,y,w,h,extra=None):
        page.append(S._placed(key,param,INDEX[param],round(x),round(y),round(w),round(h),extra=extra))
        if condition:page[-1]['bounds']['additionalInvalidatingHandles']=[f'IndexedEnabling/{condition["value"]}/2/Parameter {INDEX["pad_edit_mode"]}']
    def value_label(w,h,size,color=TEXT,align='horizontallyCentred verticallyCentred'):
        return S._value_label(0,0,w,h,size,color[1:],align,font='Roboto',style='Medium')
    for ti,tab in enumerate(tabs):
        im=Image.new('RGB',(1280,628),BG);d=ImageDraw.Draw(im)
        # Shared dark faceplate, finely ruled to match the reference cabinet.
        for yy in range(2,628,4):d.line((0,yy,1279,yy),fill='#0a1019')
        brand(d)
        page_title=tab['name'] if tab['name']=='MIDI' else tab['name'].title()
        if ti and tab['name']!='PADS':
            label(d,(1254,32),tab['name'],30,ACCENT,'rm',True)
        d.line((16,69,1264,69),fill='#41596b',width=2)
        if ti==0:
            for x in (16,1190):
                panel(d,(x,78,x+74,376))
            label(d,(53,92),'IN',22,MUTED,'mm')
            label(d,(1227,92),'OUT',22,MUTED,'mm')
            for x,w in ((16,164),(188,388),(584,220),(812,220),(1040,224)):
                panel(d,(x,490,x+w,620))
        elif ti==1:
            for column in range(6):panel(d,(24+208*column,80,216+208*column,262))
            d.line((24,274,1256,274),fill=LINE)
            d.line((24,480,1256,480),fill=LINE)
        elif ti==2:panel(d,(24,80,1256,232))
        elif tab['name']=='PADS':
            for row in range(4):
                for col in range(4):panel(d,(24+312*col,86+124*row,328+312*col,202+124*row))
            label(d,(640,604),'PLAY: TAP TO TRIGGER · ASSIGN: TURN KNOBS / Q-LINKS TO CHANGE FUNCTIONS',22,MUTED,'mm')
        elif tab['name']=='MIDI':
            panel(d,(24,80,1256,176))
            label(d,(48,116),'MIDI ASSIGNMENTS',28,TEXT,'lm',True)
            label(d,(48,152),f'ALL {len(VISIBLE_MIDI_TARGETS)} CONTROLS / NOTES + CC',22,MUTED,'lm')
            label(d,(48,552),'3  Enable Learn; press a Bank A pad or send a MIDI note / CC.',26,TEXT,'lm')
            label(d,(48,592),'Notes: toggle / cycle. CC: full range; buttons trigger on a rising edge.',24,MUTED,'lm')
        page=[];controls=[]
        bgname=f'rlo_page_{ti}.png'
        page.append(S._sub('Image',{'version':2,'imageType':'Regular','colour':'0','image':bgname},S._bounds(0,0,1280,628),'Background'))
        for w in tab['widgets']:
            kind=w['kind'];param=w['key'];condition=dict(param='pad_edit_mode',value=0 if param.startswith('pad_press_') else 1,max=1) if tab['name']=='PADS' and (param.startswith('pad_press_') or param.startswith('pad_a_')) else None;p=PARAMS[INDEX[param]];cx=w['cx'];cy=w['cy'];caption=w.get('label','')
            if kind=='button' and param.startswith('pad_press_'):
                ww,hh=w['w'],w['h'];x=cx-ww/2;y=cy-hh/2;number=int(param[-2:]);assignment=f'pad_a_{number:02}';face=f'pad_face_{number:02}'
                label(d,(x,y-86-10),caption,24,MUTED,'ls')
                file=f'rlo_padbutton_{ww}_{hh}.png';frames=2*(len(VISIBLE_MIDI_TARGETS)+1)
                if file not in assets:pad_button_art(skin/file,ww,hh);assets.add(file)
                hit=S.write_hit_png(str(skin),ww,hh);assets.add(hit);key=f'rlo_padbutton_{ww}_{hh}'
                local(key,[S._action('Mouse Down','Toggle Switch'),S._action('Enter Pressed','Toggle Switch')],[S._focus(ww,hh),
                    S._sub('Knob',{'version':1,'knobType':'FilmStrip','filmStrip':file,'numFrames':frames-1,'handleName':'Face'},S._bounds(0,0,ww,hh),'PadFace'),
                    S._value_label(12,14,ww-24,hh-20,26,TEXT[1:],'horizontallyCentred verticallyCentred',handle='Assignment',font='Roboto',style='Medium'),
                    S._button(hit,hit,1,1,ww,hh)])
                place(page,key,param,x,y,ww,hh,{'Face':INDEX[face],'Assignment':INDEX[assignment]})
                controls.append(dict(kind='padbutton',key=param,label=caption,x=x,y=y,w=ww,h=hh,strip=file,frames=frames,face=face,assignment=assignment,feedback=f'pad_indicator_{number:02}',value=[x+12,y+14,ww-24,hh-20],size=26))
            elif kind=='button':
                ww,hh=w['w'],w['h'];x=cx-ww/2;y=cy-hh/2
                boolean=p['kind']!='trigger';base=f'rlo_button_{param}_{ww}_{hh}'
                for state in ('off','on'):
                    name=f'{base}_{state}.png';button_art(skin/name,ww,hh,caption,param,state=='on',boolean);assets.add(name)
                key=base
                actions=[S._action('Mouse Down','Toggle Switch'),S._action('Enter Pressed','Toggle Switch')]
                feedback=BUTTON_FEEDBACK.get(param)
                badge=f'pad_label_{param}' if param in PAD_BADGES else None
                extra={'Pad':INDEX[badge]} if badge else None
                primary=hh>=84 and param in ('record','play','dub','stop_now','clear')
                bx,by,bw,bh=(ww-64,6,56,24) if primary else ((ww-58,6,50,24) if hh>=112 else (ww/2-40,2,80,24))
                def children(picture_on,picture_off):
                    result=[S._focus(ww,hh),S._button(picture_on,picture_off,1,1,ww,hh)]
                    if badge:
                        result.append(S._value_label(bx,by,bw,bh,22,MUTED[1:],'horizontallyCentred verticallyCentred',handle='Pad',font='Roboto',style='Medium'))
                        hit=S.write_hit_png(str(skin),ww,hh);assets.add(hit);result.append(S._button(hit,hit,1,1,ww,hh))
                    return result
                if feedback:
                    # The action still writes the released trigger parameter.
                    # A separate engine readout selects the persistent lamp,
                    # including pending blinks; trigger spring-back cannot dim it.
                    for lit in range(2):
                        variant=key+f'_lamp_{lit}';picture=base+('_on.png' if lit else '_off.png')
                        local(variant,actions,children(picture,picture))
                        place(page,variant,param,x,y,ww,hh,extra)
                        page[-1]['bounds']['additionalInvalidatingHandles']=[f'IndexedEnabling/{lit}/2/Parameter {INDEX[feedback]}']
                else:
                    local(key,actions,children(base+'_on.png',base+'_off.png'))
                    place(page,key,param,x,y,ww,hh,extra)
                badge_info=dict(key=badge,x=x+bx,y=y+by,w=bw,h=bh,size=22,color=MUTED) if badge else None
                controls.append(dict(kind='button',key=param,label=caption,x=x,y=y,w=ww,h=hh,boolean=boolean,feedback=feedback,badge=badge_info,off=base+'_off.png',on=base+'_on.png'))
            elif kind.startswith('enum'):
                options=p['options'];n=len(options);sw,sh=w['sw'],w['sh'];gap=8
                total=n*sw+(n-1)*gap;x=cx-total/2;y=cy-sh/2
                if caption:label(d,(x,y-86-14),caption,24,MUTED,'ls')
                for option,caption_ in enumerate(options):
                    caption_={'Double tap':'Double tap','Immediate':'Immediate'}.get(caption_,caption_)
                    if param=='auto_start':caption_='AUTO ON' if option else 'AUTO OFF'
                    name=f'rlo_segment_{param}_{option}_{sw}_{sh}'
                    for state in ('off','on'):
                        file=f'{name}_{state}.png';segment_art(skin/file,sw,sh,caption_,state=='on',PAD_COLOURS.get(param,ACCENT));assets.add(file)
                    local(name,[S._action('Mouse Down','Q-Link')],[S._button(name+'_on.png',name+'_off.png',option,n,sw,sh)])
                    place(page,name,param,x+option*(sw+gap),y,sw,sh)
                    controls.append(dict(kind='option',key=param,label=caption_,x=x+option*(sw+gap),y=y,w=sw,h=sh,option=option,off=name+'_off.png',on=name+'_on.png'))
            elif kind=='readout':
                ww,hh=w['w'],w['h'];x=cx-ww/2;y=cy-hh/2
                size=(24 if ti==0 else 28) if param in ('status','duration','position','clock') else 26 if param=='binding' else 22
                align='left verticallyCentred' if param in ('message','in_meter','out_meter') else 'horizontallyCentred verticallyCentred'
                if caption:label(d,(x,y-86-12),caption,22,MUTED,'ls')
                color=ACCENT if param in ('status','clock') else TEXT if param!='message' else MUTED
                key=f'rlo_value_{param}_{ww}_{hh}'
                colors=['#91aec6','#ff2453','#00ffd5','#ffc800','#d6e2ee','#ffc800','#91aec6','#ffc800','#ffc800','#ffc800','#ffc800'] if param=='status' else None
                if colors:
                    for state,ink in enumerate(colors):
                        variant=key+f'_{state}';local(variant,[],[value_label(ww,hh,size,ink,align)]);place(page,variant,param,x,y,ww,hh)
                        page[-1]['bounds']['additionalInvalidatingHandles']=[f'IndexedEnabling/{state}/11/Parameter {INDEX[param]}']
                else:local(key,[],[value_label(ww,hh,size,color,align)]);place(page,key,param,x,y,ww,hh)
                controls.append(dict(kind='readout',key=param,x=x,y=y,w=ww,h=hh,size=size,color=color,colors=colors,align='left' if align.startswith('left') else 'center'))
            elif kind=='knob':
                r=w['r'];s=2*r+10;ww=w['bw'];compact=bool(w.get('compact'));side=w.get('lay')=='side' and not compact
                assignment=bool(p.get('pad_assignment'));frames=KNOB_FRAMES
                file=f'rlo_knob_{r}.png'
                if compact:
                    hh=124;x=cx-ww/2;y=cy-hh/2
                    label(d,(cx,y-86+13),caption,24,TEXT,'mm')
                    children=[S._sub('Knob',{'version':1,'knobType':'FilmStrip','filmStrip':f'rlo_knob_{r}.png','numFrames':KNOB_FRAMES-1,'handleName':'Data'},S._bounds((ww-s)//2,28,s,s),'Knob'),S._value_label(0,96,ww,28,24,TEXT[1:],'horizontallyCentred verticallyCentred',font='Roboto',style='Medium')]
                    vr=(x,y+96,ww,28)
                elif side:
                    hh=w['bh'];x=cx-ww/2;y=cy-hh/2;
                    if caption:label(d,(x,y-86-12+(8 if assignment else 0)),caption,24,MUTED,'ls')
                    children=[S._sub('Knob',{'version':1,'knobType':'FilmStrip','filmStrip':file,'numFrames':frames-1,'handleName':'Data'},S._bounds(0,(hh-s)//2,s,s),'Knob')]
                    val=S._value_label(s+12,0,ww-s-12,hh,22 if assignment else 26 if ti==0 else 36,TEXT[1:],'horizontallyCentred verticallyCentred',font='Roboto',style='Medium');children.append(val)
                    vr=(x+s+12,y,ww-s-12,hh)
                else:
                    hh=s+88;x=cx-ww/2;y=cy-s/2-38
                    label(d,(cx,y-86+18),caption,24,TEXT,'mm')
                    children=[S._sub('Knob',{'version':1,'knobType':'FilmStrip','filmStrip':f'rlo_knob_{r}.png','numFrames':KNOB_FRAMES-1,'handleName':'Data'},S._bounds((ww-s)//2,38,s,s),'Knob'),S._value_label(0,s+46,ww,40,28,TEXT[1:],'horizontallyCentred verticallyCentred',font='Roboto',style='Medium')]
                    vr=(x,y+s+46,ww,40)
                if file not in assets:knob_art(skin/file,r);assets.add(file)
                key=f'rlo_knob_control_{r}_{ww}_{hh}_{int(side)}'
                local(key,[S._action('Mouse Down','Q-Link'),S._action('Double Click','Show Overlay','knob overlay')],[S._focus(ww,hh)]+children)
                place(page,key,param,x,y,ww,hh)
                controls.append(dict(kind='knob',key=param,label=caption or p['name'],x=x,y=y,w=ww,h=hh,strip=file,frames=frames,art=[x if side else cx-s/2,y+28 if compact else y+(hh-s)//2 if side else cy-s/2,s,s],value=list(vr),size=22 if assignment else 24 if compact else 26 if side and ti==0 else 36 if side else 28))
            elif kind=='slider_h':
                ww,hh=w['w'],w['h'];x=cx-ww/2;y=cy-hh/2
                label(d,(x,y-86-14),caption,24,MUTED,'ls')
                file=f'rlo_slider_{ww}_{hh}.png';frames=64;atlas=Image.new('RGBA',(ww,hh*frames))
                for frame in range(frames):
                    cell=Image.new('RGBA',(ww,hh));cd=ImageDraw.Draw(cell);pos=24+round((ww-48)*frame/(frames-1))
                    cd.rounded_rectangle((12,hh/2-4,ww-12,hh/2+4),radius=4,fill='#527795')
                    cd.rounded_rectangle((12,hh/2-4,pos,hh/2+4),radius=4,fill=ACCENT)
                    cd.rounded_rectangle((pos-14,8,pos+14,hh-8),radius=5,fill='#e4ecf7',outline='#ffffff');atlas.alpha_composite(cell,(0,frame*hh))
                atlas.save(skin/file);assets.add(file)
                key=f'rlo_slider_{ww}_{hh}'
                local(key,[S._action('Mouse Down','Q-Link')],[S._focus(ww,hh),S._sub('Knob',{'version':1,'knobType':'FilmStrip','filmStrip':file,'numFrames':frames,'handleName':'Data'},S._bounds(0,0,ww,hh),'Slider')])
                place(page,key,param,x,y,ww,hh)
                controls.append(dict(kind='slider',key=param,label=caption or p['name'],x=x,y=y,w=ww,h=hh,strip=file,frames=frames))
            if condition:controls[-1]['condition']=condition
        if ti==1:
            label(d,(32,614),'INPUT',22,MUTED,'lm');label(d,(680,614),'OUTPUT',22,MUTED,'lm')
        im.save(skin/bgname);assets.add(bgname)
        pd['tabs'][ti]['componentDefinition']['componentsData']=page
        manifest['tabs'].append(dict(name=page_title,widgets=controls,background=bgname))
    manifest['assets']=sorted(assets)
    S.to_mpc2x(tui);(skin/'TUI.json').write_text(json.dumps(tui,indent=2)+'\n')
    (ROOT/'preview/controls.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest

def native_touch_controls(skin):
    """MPC OS 3.x explicitly supports the modern knob drag-direction setting."""
    p=Path(skin)/'TUI.json';doc=json.loads(p.read_text());count=0
    for definition in doc['pageData']['componentDefinitions']['localComponentDefinitions']:
        key=definition['key']
        rotary=key.startswith('rlo_knob_control_');linear=key.startswith('rlo_slider_')
        if not rotary and not linear:continue
        for child in definition['value'].get('componentsData',[]):
            cd=child.get('componentData',{})
            if cd.get('type')!='Knob':continue
            cd['data'].update(version=5,invert=False,dragOrientation='Vertical' if rotary else 'Horizontal');count+=1
    p.write_text(json.dumps(doc,indent=2)+'\n')
    return count


def preview(skin,S,manifest):
    """Clean native-layout PNGs. Live values are overlaid by the actual test host in the browser."""
    skin=Path(skin)
    def default(key):
        if key.startswith('pad_face_'):return PARAMS[INDEX['pad_a_'+key[-2:]]]['default']*2/(2*len(VISIBLE_MIDI_TARGETS)+1)
        p=PARAMS[INDEX[key]];return (p['default']-p['min'])/(p['max']-p['min'])
    def default_text(key):
        known={'status':'EMPTY','duration':'0.00 SEC','position':'000 %','clock':'120.0 BPM  |  1','message':'READY TO RECORD',
               'in_meter':'-100.0 dB','out_meter':'-100.0 dB','binding':'NOTE 36 / CC 20',
               'hp_cutoff':'OFF','lp_cutoff':'OFF','sequence_info':'SEQ 4 > 4 B','host_transport':'MPC STOP','button_status':'TOUCH + PAD READY'}
        if key in known:return known[key]
        if key.startswith('pad_label_'):
            action=key.removeprefix('pad_label_');return f'A{PAD_DEFAULTS.index(action)+1}' if action in PAD_DEFAULTS else '-'
        if key=='pad_status':return 'PAD BANK A READY'
        p=PARAMS[INDEX[key]];v=p['default']
        if p.get('options'):return p['options'][round(v)]
        if key in ('input_gain','volume','master'):return f'{20*math.log10(max(.00001,v)):.1f} dB'
        if key=='tempo':return f'{v:.1f} BPM'
        if key in ('feedback','click_gain'):return f'{v*100:.0f} %'
        if key=='crossfade_ms':return f'{v:.1f} ms'
        if key=='start_threshold':return f'{v:.1f} dB'
        if key=='pan':return 'C' if v==0 else f'{abs(v)*100:.0f} '+('L' if v<0 else 'R')
        if key=='bars':return f'{int(v)} bars'
        if key=='file_slot':return f'Slot {int(v):02}'
        return f'{v:.0f}' if p['kind']=='integer' else f'{v:.2f}'
    def strip(im,file,rect,frames,value):
        x,y,w,h=map(round,rect);atlas=Image.open(skin/file).convert('RGBA');fh=atlas.height//frames
        k=round(max(0,min(1,value))*(frames-1));cell=atlas.crop((0,k*fh,atlas.width,(k+1)*fh)).resize((w,h),Image.Resampling.LANCZOS)
        im.alpha_composite(cell,(x,y-86))
    visuals=json.loads((ROOT/'preview/visuals.json').read_text())
    for ti,tab in enumerate(manifest['tabs']):
        for mode in (range(3) if ti==0 else (0,)):
            im=Image.open(skin/tab['background']).convert('RGBA')
            for c in tab['widgets']:
                if c.get('condition') and default(c['condition']['param'])!=c['condition']['value']:continue
                if c['kind'] in ('button','option'):
                    selected=bool(default(c.get('feedback') or c['key'])>.5) if c['kind']=='button' else round(default(c['key'])*(len(PARAMS[INDEX[c['key']]]['options'])-1))==c['option']
                    if c['key']=='view':selected=mode==c['option']
                    img=Image.open(skin/c['on' if selected else 'off']).convert('RGBA');im.alpha_composite(img,(round(c['x']),round(c['y']-86)))
                elif c['kind']=='padbutton':strip(im,c['strip'],[c[k] for k in ('x','y','w','h')],c['frames'],default(c['face']))
                elif c['kind']=='knob':strip(im,c['strip'],c['art'],c['frames'],default(c['key']))
                elif c['kind']=='slider':strip(im,c['strip'],[c[k] for k in ('x','y','w','h')],c['frames'],default(c['key']))
            if ti==0:
                for c in visuals['drawables']:
                    if c['view'] not in (-1,mode):continue
                    if c.get('condition') and round(default(c['condition']['param'])*c['condition']['max'])!=c['condition']['value']:continue
                    if c.get('frames'):strip(im,c['image'],[c[k] for k in ('x','y','w','h')],c['frames'],default(c['param']))
                    else:im.alpha_composite(Image.open(skin/c['image']).convert('RGBA'),(c['x'],c['y']-86))
            dr=ImageDraw.Draw(im)
            for c in tab['widgets']:
                if c.get('condition') and default(c['condition']['param'])!=c['condition']['value']:continue
                if c['kind']=='readout':
                    x,y,w,h=(c[k] for k in ('x','y','w','h'));color=c.get('colors',[c['color']])[0] if c.get('colors') else c['color']
                    label(dr,(x if c['align']=='left' else x+w/2,y-86+h/2),default_text(c['key']),c['size'],color,'lm' if c['align']=='left' else 'mm')
                elif c['kind']=='padbutton':
                    x,y,w,h=c['value'];label(dr,(x+w/2,y-86+h/2),default_text(c['assignment']),c['size'],TEXT,'mm')
                elif c['kind']=='knob':
                    x,y,w,h=c['value'];label(dr,(x+w/2,y-86+h/2),default_text(c['key']),c['size'],TEXT,'mm')
            for c in tab['widgets']:
                if c.get('badge'):
                    b=c['badge'];label(dr,(b['x']+b['w']/2,b['y']-86+b['h']/2),default_text(b['key']),b['size'],MUTED,'mm')
            file=f'skin-{ti}.png' if mode==0 else f'skin-{ti}_view-{mode}.png'
            im.convert('RGB').save(ROOT/'preview'/file)
