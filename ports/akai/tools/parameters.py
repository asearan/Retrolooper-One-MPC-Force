# Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
# SPDX-License-Identifier: GPL-3.0-only
"""Stable VST indices: append parameters; never reorder released indices."""
import json
import colorsys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PARAMS = []

def add(key, name, lo=0, hi=1, default=0, options=None, kind="float"):
    p = dict(key=key, name=name, min=lo, max=hi, default=default)
    if options:
        p.update(options=options, min=0, max=len(options)-1)
    if kind == "trigger":
        p.update(momentary=True)
    if kind == "readout":
        p.update(type="readout", display="string", live=True)
    p["kind"] = kind
    PARAMS.append(p)

for key, name in [("record", "Record"), ("play", "Play / Stop"), ("dub", "Overdub"), ("mute", "Mute"),
                  ("undo", "Undo"), ("redo", "Redo"), ("clear", "Clear"), ("stop", "Stop"),
                  ("stop_now", "Stop Now"), ("sync_start", "Sync to Start")]:
    add(key, name, kind="trigger")
add("tempo", "Tempo", 40, 240, 120)
add("master", "Master", default=1)
add("monitor", "Input Monitor", default=1, options=["Off", "On"])
add("host_sync", "Host Sync", default=1, options=["Off", "On"])
add("timing", "Global Timing", options=["Immediate", "Bar", "Loop"])
add("record_mode", "Record Mode", options=["Off", "On"])
add("mute_mode", "Mute Mode", options=["Off", "On"])
add("input_gain", "Input Gain", 0, 4, 1)
add("volume", "Loop Volume", default=1)
add("pan", "Pan", -1, 1, 0)
add("input", "Record Input", options=["Stereo", "Left", "Right", "Off"])
add("output", "Loop Output", options=["Channel", "Off"])
add("crossfader", "A/B Crossfader", default=.5)
add("assign", "Crossfade Assign", options=["Off", "A", "B"])
add("feedback", "Dub Feedback", default=1)
add("crossfade_ms", "Loop Fade", 0, 20, 5)
add("click", "Click", options=["Off", "On"])
add("click_gain", "Click Level", default=.5)
for key, label in [("stop_timing", "Stop Timing"), ("play_timing", "Play Timing"), ("rec_timing", "Record Timing"),
                   ("dub_timing", "Dub Timing"), ("mute_timing", "Mute Timing")]:
    add(key, label, options=["Global", "Immediate", "Bar", "Loop"])
add("after_record", "After Record", options=["Play", "Overdub"])
add("bars", "Target Bars", 1, 64, 4, kind="integer")
for key, label in [("reverse", "Reverse"), ("half", "Half Length"), ("double_length", "Double Length"),
                   ("trim", "Trim One Bar"), ("restore", "Restore One Bar"), ("set_bars", "Set Bars"),
                   ("import_audio", "Import WAV"), ("export_audio", "Export WAV")]:
    add(key, label, kind="trigger")
add("file_slot", "WAV Slot", 1, 64, 1, kind="integer")
add("learn", "MIDI Learn", options=["Off", "On"])
add("learn_target", "Learn Target", options=["Record", "Play", "Overdub", "Mute", "Undo", "Redo", "Clear"])
add("gesture", "Learn Gesture", options=["Tap", "Double tap", "Hold"])
add("forget", "Forget Binding", kind="trigger")
for key, name in [("status", "Loop State"), ("duration", "Loop Seconds"), ("position", "Position"),
                  ("clock", "Clock"), ("message", "Message"), ("binding", "MIDI Binding"),
                  ("in_meter", "Input Peak"), ("out_meter", "Output Peak")]:
    add(key, name, kind="readout")

# Append only: the first 56 indices belong to v0.1.0 projects.
add("view", "Visual View", options=["Waveform", "Cassette", "Meters"])
for side in ("lo", "hi"):
    for column in range(48):
        add(f"wave_{side}_{column:02}", f"Wave {side} {column+1}", kind="readout")
add("reel_phase", "Cassette Reels", kind="readout")
for path in ("input", "output"):
    for measure in ("peak", "rms"):
        for channel in ("l", "r"):
            add(f"{path}_{measure}_{channel}", f"{path} {measure} {channel}", kind="readout")

# Append only: native mosaic ballistics and mapping navigation.
for key,name in [("focus_meter","Track Meter"),("focus_hold","Meter Peak Hold"),("focus_clip","Meter Clip")]:
    add(key,name,kind="readout")
add("target_previous","Previous MIDI Target",kind="trigger")
add("target_next","Next MIDI Target",kind="trigger")
# Append only: independent physical cassette reels; interactive indices stay fixed.
add("takeup_phase", "Cassette Take-up Reel", kind="readout")
add("tape_fill", "Cassette Tape Pack", default=.25, kind="readout")
# MPC transport integration. Append only; v0.1.8 has 169 parameters.
add("length_ratio", "Sequence Length", default=1, options=["/2", "x1", "x2"])
for key,name in [("sequence_info","MPC Sequence"),("host_transport","MPC Transport"),("host_click","MPC Click"),("host_monitor","MPC Monitoring")]:
    add(key,name,kind="readout")
add("capture_source", "Capture Control", default=1, options=["MPC Rec", "Touch/Pads"])
add("button_status", "Capture Controls", kind="readout")
# Append only: controls for the host-driven audio-track workflow.
add("hp_cutoff", "Loop High Pass", default=0)
add("lp_cutoff", "Loop Low Pass", default=1)
add("launch_mode", "Loop Launch", options=["Sequence", "Next bar"])
add("saved_active_span", "Saved Active Frames", 0, 8388608, 0, kind="readout")
add("clock_health", "Clock Health", kind="readout")
# Append only: threshold first-take start; released MIDI slots stay in place.
add("auto_start", "Auto Start", options=["Off", "On"])
add("start_threshold", "Start Threshold", -72, 0, -36)
for key, name in [("record_indicator", "Record Light"), ("play_indicator", "Play Light"),
                  ("dub_indicator", "Overdub Light"), ("mute_indicator", "Mute Light"),
                  ("stop_indicator", "Stop Light")]:
    add(key, name, kind="readout")
add("pad_bank_enabled", "Pad Bank A", default=1, options=["Off", "On"])
MIDI_TARGETS=[p for p in PARAMS if p["kind"]!="readout"]
# Keep old binding slots intact in saved projects, while retiring these controls
# from the actual MIDI Learn picker and the hardware interface.
RETIRED_KEYS={"tempo","monitor","host_sync","click","click_gain","crossfader","assign"}
VISIBLE_MIDI_TARGETS=[p for p in MIDI_TARGETS if p['key'] not in RETIRED_KEYS]
PAD_DEFAULTS=['record','play','dub','mute','undo','redo','clear','reverse','stop_now','auto_start','export_audio','sync_start','length_ratio','view','input','output']
PAD_COLOURS=dict(zip(PAD_DEFAULTS,['#ff454e','#55f15a','#ffbd37','#ff7d29','#19c6ad','#ff5db2','#b85cff','#ad9bff','#3b93ff','#e4ed43','#e6f1ed','#37d9ff','#ef7f78','#a5d93d','#6bc4e8','#6274ef']))
for i,p in enumerate(VISIBLE_MIDI_TARGETS):
    if p['key'] not in PAD_COLOURS:
        rgb=colorsys.hsv_to_rgb((i*.61803398875)%1,.55,1)
        PAD_COLOURS[p['key']]='#'+''.join(f'{round(c*255):02x}' for c in rgb)
# Assignment selectors are configuration, not recursive pad action targets.
# Freeze the performance target/binding list above before appending them.
for pad,default in enumerate(PAD_DEFAULTS,1):
    add(f'pad_a_{pad:02}',f'Pad A{pad}',default=1+next(i for i,p in enumerate(VISIBLE_MIDI_TARGETS) if p['key']==default),
        options=['Off']+[p['name'] for p in VISIBLE_MIDI_TARGETS],kind='integer')
    PARAMS[-1]['pad_assignment']=True
add('pad_status','Pad Controller',kind='readout')
PAD_BADGES=['record','play','dub','mute','undo','redo','clear','reverse','stop_now','export_audio']
for key in PAD_BADGES:add(f'pad_label_{key}',f'{key} pad',kind='readout')
# Touch pad triggers and display feedback are not recursive performance targets.
add('clear_indicator','Loop Present',kind='readout')
for pad in range(1,17):add(f'pad_press_{pad:02}',f'Pad A{pad}',kind='trigger')
for pad in range(1,17):add(f'pad_indicator_{pad:02}',f'Pad A{pad} Light',kind='readout')
for pad in range(1,17):add(f'pad_face_{pad:02}',f'Pad A{pad} Face',kind='readout')
add("pad_edit_mode","Pad Page Mode",options=["Play","Assign"])
# Keep the original seven target indices for existing assignments.
for p in PARAMS:
    if p["key"]=="learn_target":
        p["options"]=[t["name"] for t in VISIBLE_MIDI_TARGETS];p["max"]=len(VISIBLE_MIDI_TARGETS)-1

def generate():
    old = ROOT / "params.lock"
    keys = [p["key"] for p in PARAMS]
    if old.exists() and keys[:len(old.read_text().split())] != old.read_text().split():
        raise SystemExit("Parameter order changed; append new parameters instead")
    old.write_text("\n".join(keys)+"\n")
    (ROOT/"params.json").write_text(json.dumps(dict(name="RetroLooper One", params=PARAMS), indent=2)+"\n")
    lines = ["// Generated by tools/parameters.py; stable VST parameter order.", "#pragma once", "namespace akai {",
             "enum Param {" + ",".join(keys) + ",param_count};",
             "struct Parameter {const char* name;float low,high,initial;int kind;const char* options;};",
             "inline constexpr Parameter parameters[] = {"]
    for p in PARAMS:
        vals = [json.dumps(p["name"]), f'{float(p["min"])}f', f'{float(p["max"])}f', f'{float(p["default"])}f',
                str(["float","integer","trigger","readout"].index(p["kind"])), json.dumps("|".join(p.get("options",[])))]
        lines.append("{"+",".join(vals)+"},")
    visible=[MIDI_TARGETS.index(p) for p in VISIBLE_MIDI_TARGETS]
    lines.extend(["};", "inline constexpr int midiTargets[] = {"+",".join(p["key"] for p in MIDI_TARGETS)+"};", "inline constexpr int midiTargetCount = sizeof(midiTargets)/sizeof(midiTargets[0]);",
        "inline constexpr int midiVisibleTargets[] = {"+','.join(map(str,visible))+"};",
        "inline constexpr int midiVisibleCount = sizeof(midiVisibleTargets)/sizeof(midiVisibleTargets[0]);", "}"])
    (ROOT/"src"/"Parameters.h").write_text("\n".join(lines)+"\n")
    palette=['#070c13']+[PAD_COLOURS[p['key']] for p in VISIBLE_MIDI_TARGETS]
    pad_lines=['#pragma once','#include <cstdint>','namespace akai {',
        'inline constexpr std::uint8_t padColours[][3] = {']
    pad_lines += ['{'+','.join(str(int(c[i:i+2],16)) for i in (1,3,5))+'},' for c in palette]
    pad_lines += ['};','inline constexpr int padBadgeFunctions[] = {'+','.join(PAD_BADGES)+'};','}']
    (ROOT/'src/PadBank.h').write_text('\n'.join(pad_lines)+'\n')

if __name__ == "__main__":
    generate()
