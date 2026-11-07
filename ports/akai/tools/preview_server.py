#!/usr/bin/env python3
# Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
# SPDX-License-Identifier: GPL-3.0-only
"""Loopback-only, offline native-engine testbench: python3 tools/preview_server.py"""
import argparse
import atexit
import ctypes as C
from http.server import HTTPServer, BaseHTTPRequestHandler
import io
import json
import math
import os
from pathlib import Path
import struct
import sys
import time
import urllib.parse
import wave

ROOT=Path(__file__).resolve().parents[1]
os.environ["RETROLOOPER_TEST_LOOPS"]=str(ROOT/"build/preview-loops")
sys.path.insert(0,str(ROOT/"tools"))
from parameters import PARAMS, PAD_COLOURS, VISIBLE_MIDI_TARGETS
INDEX={p["key"]:i for i,p in enumerate(PARAMS)}
lib=C.CDLL(str(ROOT/"build/preview-engine.dylib"),mode=C.RTLD_GLOBAL)
lib.rlo_create.restype=C.c_void_p
lib.rlo_close.argtypes=[C.c_void_p]
lib.rlo_host_physical_rec.argtypes=[C.c_void_p]
lib.rlo_host_transport.argtypes=[C.c_void_p,C.c_int,C.c_int]
lib.rlo_host_recording.argtypes=[C.c_void_p,C.c_int]
lib.rlo_host_sequence.argtypes=[C.c_void_p,C.c_int]
lib.rlo_pad.argtypes=[C.c_void_p,C.c_int]
lib.rlo_midi.argtypes=[C.c_void_p,C.c_int,C.c_int,C.c_int]
lib.rlo_set.argtypes=[C.c_void_p,C.c_int,C.c_float]
lib.rlo_get.argtypes=[C.c_void_p,C.c_int];lib.rlo_get.restype=C.c_float
lib.rlo_display.argtypes=[C.c_void_p,C.c_int,C.c_char_p]
lib.rlo_render.argtypes=[C.c_void_p,C.c_int,C.c_float,C.c_double,C.POINTER(C.c_float)]
lib.rlo_save.argtypes=[C.c_void_p];lib.rlo_reload.argtypes=[C.c_void_p]
handle=lib.rlo_create()
if not handle:raise SystemExit("Native engine could not start")
atexit.register(lambda:lib.rlo_close(handle))
latest_audio=b""
latest_timeline=[]

def setting(key,value):
    p=PARAMS[INDEX[key]]
    lib.rlo_set(handle,INDEX[key],(value-p["min"])/(p["max"]-p["min"]))

def tap(key):
    setting(key,1);setting(key,0)

def render(seconds=.003,amplitude=0,frequency=220):
    global latest_audio,latest_timeline
    frames=min(44100*4,max(128,int(seconds*44100)))
    data=(C.c_float*(frames*2))()
    timeline=[]
    if frames>4410:
        timeline.append(dict(at=0,values={p['key']:lib.rlo_get(handle,i) for i,p in enumerate(PARAMS) if p['kind']=='readout'}))
    for start in range(0,frames,4410):
        count=min(4410,frames-start)
        lib.rlo_render(handle,count,amplitude,frequency,C.cast(C.byref(data,start*8),C.POINTER(C.c_float)))
        if frames>4410:
            timeline.append(dict(at=(start+count)/44100,values={p['key']:lib.rlo_get(handle,i) for i,p in enumerate(PARAMS) if p['kind']=='readout'}))
    if frames>4410:
        buf=io.BytesIO()
        with wave.open(buf,"wb") as w:
            w.setparams((2,2,44100,frames,"NONE","not compressed"))
            w.writeframes(b"".join(struct.pack("<h",int(max(-1,min(1,v))*32767)) for v in data))
        latest_audio=buf.getvalue()
        latest_timeline=timeline

# Preserve Host Sync On and publish the first controller feedback snapshot.
render(.12)

def state():
    values={}
    for i,p in enumerate(PARAMS):
        b=C.create_string_buffer(24);lib.rlo_display(handle,i,b)
        values[p["key"]]={"normalized":lib.rlo_get(handle,i),"text":b.value.decode("utf-8",errors="replace")}
    return dict(values=values,audio=bool(latest_audio),timeline=latest_timeline)

class Handler(BaseHTTPRequestHandler):
    def send(self,body,kind="application/json",code=200):
        self.send_response(code);self.send_header("Content-Type",kind);self.send_header("Content-Length",str(len(body)))
        self.send_header("Cache-Control","no-store");self.send_header("X-Content-Type-Options","nosniff");self.end_headers();self.wfile.write(body)
    def do_GET(self):
        route=urllib.parse.urlsplit(self.path).path
        if route=="/api/state":return self.send(json.dumps(state()).encode())
        if route=="/api/config":
            controls=json.loads((ROOT/"preview/controls.json").read_text())
            visuals=json.loads((ROOT/"preview/visuals.json").read_text())
            return self.send(json.dumps(dict(tabs=controls['tabs'],params=PARAMS,visuals=visuals,pad_functions=[dict(key=p['key'],name=p['name'],colour=PAD_COLOURS[p['key']]) for p in VISIBLE_MIDI_TARGETS])).encode())
        if route=="/audio.wav":return self.send(latest_audio,"audio/wav")
        if route=="/":path=ROOT/"tools/preview.html"
        elif route.startswith("/skin-") and route.endswith(".png") and route[6:-4].isdigit():path=ROOT/"preview"/route[1:]
        elif route=="/font.ttf":path=ROOT/"art/PressStart2P-Regular.ttf"
        elif route=="/ui-font.ttf":path=ROOT/"art/InterVariable.ttf"
        elif route.startswith('/asset/') and Path(route).name in set(json.loads((ROOT/'preview/controls.json').read_text())['assets']):
            path=ROOT/'build/skin/RetroLooper - VST - RetroLooper One/Plugin Skins'/Path(route).name
        elif route.startswith('/visual/') and Path(route).name in {d['image'] for d in json.loads((ROOT/'preview/visuals.json').read_text())['drawables']}:
            path=ROOT/'build/skin/RetroLooper - VST - RetroLooper One/Plugin Skins'/Path(route).name
        else:return self.send(b"Not found","text/plain",404)
        if not path.is_file():return self.send(b"Not found","text/plain",404)
        return self.send(path.read_bytes(),"text/html; charset=utf-8" if path.suffix==".html" else "font/ttf" if path.suffix==".ttf" else "image/png")
    def do_POST(self):
        # A normal local web page cannot post commands cross-origin.
        origin=self.headers.get("Origin")
        if origin and origin!="http://"+self.headers.get("Host",""):return self.send(b"Forbidden","text/plain",403)
        if urllib.parse.urlsplit(self.path).path!="/api/action":return self.send(b"Not found","text/plain",404)
        length=int(self.headers.get("Content-Length","0"))
        if length<1 or length>4096:return self.send(b"Invalid payload","text/plain",400)
        try:
            payload=json.loads(self.rfile.read(length));command=payload.get("command","")
            if command=="tone":
                tap("stop_now");render();tap("clear");render();setting("timing",0)
                lib.rlo_host_transport(handle,0,1);lib.rlo_host_sequence(handle,1);render();time.sleep(.05);tap("record");render();lib.rlo_host_recording(handle,0);lib.rlo_host_transport(handle,1,1);render(2,.22,220);render();lib.rlo_host_recording(handle,0);render();render(2)
            elif command=="host_play":lib.rlo_host_transport(handle,1,1);render()
            elif command=="host_record":lib.rlo_host_physical_rec(handle);lib.rlo_host_recording(handle,1);lib.rlo_host_transport(handle,1,1);render()
            elif command=="host_overdub":lib.rlo_host_recording(handle,1);lib.rlo_host_transport(handle,1,0);render()
            elif command=="host_record_off":lib.rlo_host_recording(handle,0);render()
            elif command=="host_stop":lib.rlo_host_transport(handle,0,0);render()
            elif command=="host_sequence":
                lib.rlo_host_sequence(handle,max(1,min(64,int(payload.get("bars",4)))));render()
            elif command=="overdub":
                tap("dub");render(2,.10,330);tap("dub");render();render(2)
            elif command=="listen":render(2,.2 if payload.get("input") else 0,220)
            elif command=="save":
                if not lib.rlo_save(handle):raise ValueError("Project snapshot failed")
            elif command=="reload":
                if not lib.rlo_reload(handle):raise ValueError("Save a project snapshot first")
                render()
            elif command=="pad":
                pad=int(payload["pad"])
                if not 1<=pad<=16:raise ValueError("Pad range is 1–16")
                lib.rlo_pad(handle,pad-1);render();time.sleep(.03);render()
            elif command=="midi":
                number=int(payload["number"]);value=int(payload.get("value",127))
                if not 0<=number<=127 or not 0<=value<=127 or payload.get("type") not in ("note","cc"):raise ValueError("MIDI range is 0–127")
                note=payload["type"]=="note"
                lib.rlo_midi(handle,0x90 if note else 0xb0,number,127 if note else value);render()
                if note:lib.rlo_midi(handle,0x80,number,0);render()
            elif command=="set":
                key=payload["key"];v=float(payload["value"])
                if key not in INDEX or not math.isfinite(v):raise ValueError("Invalid parameter")
                setting(key,max(PARAMS[INDEX[key]]["min"],min(PARAMS[INDEX[key]]["max"],v)));render()
            elif command=="tap":
                key=payload["key"]
                if key not in INDEX or PARAMS[INDEX[key]]["kind"]!="trigger":raise ValueError("Invalid trigger")
                tap(key);render();time.sleep(.03)
            else:raise ValueError("Unknown command")
            return self.send(json.dumps(state()).encode())
        except (ValueError,KeyError,TypeError) as e:return self.send(json.dumps(dict(error=str(e))).encode(),code=400)
    def log_message(self,fmt,*args):
        if args and str(args[0]).startswith("POST"):print(fmt%args,flush=True)

if __name__=="__main__":
    parser=argparse.ArgumentParser();parser.add_argument("--port",type=int,default=8765);a=parser.parse_args()
    server=HTTPServer(("127.0.0.1",a.port),Handler)
    print(f"RetroLooper One native testbench: http://127.0.0.1:{a.port}",flush=True)
    server.serve_forever()
