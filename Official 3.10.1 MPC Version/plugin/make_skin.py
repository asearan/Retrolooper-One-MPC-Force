#!/usr/bin/env python3
"""Build the official Retrolooper One skin from the original Looper, Mixer and Timing art.

Pads and the MIDI page are not included. Controls that this plugin does not
perform (import, trim, pad assign, per-action timing grids) are not drawn.
"""

import os
import shutil
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
ART = Path("/Volumes/Ally SSD/App Development/Retrolooper/Source/MAC BUILD/DAW ONE/skin")
FONT = Path("/Volumes/Ally SSD/App Development/Retrolooper/Source/MAC BUILD/AKAI BUILD/art/VT323-Regular.ttf")
OUT = ROOT / "skins"

P = {
    "master": 0, "loop": 1, "input": 2, "pan": 3, "play": 4, "record": 5,
    "dub": 6, "mute": 7, "length": 8, "bars": 9, "launch": 10, "follow": 11,
    "after": 12, "source": 13, "output": 14, "feedback": 15, "fade": 16,
    "hp": 17, "lp": 18, "auto": 19, "thresh": 20, "undo": 21, "redo": 22,
    "clear": 23, "reverse": 24, "stop": 25, "sync": 26, "export": 27,
}

CHOICES = {
    8: ["/2", "x1", "x2"],
    10: ["Sequence", "Next bar"],
    12: ["Play", "Overdub"],
    13: ["Stereo", "Left", "Right", "Off"],
}
BOOLS = [4, 5, 6, 7, 11, 14, 19, 21, 22, 23, 24, 25, 26, 27]


def param(index):
    return "Parameter %d" % index


def bounds(x, y, w, h, focus="Yes", invalidating=None):
    box = {
        "version": 2 if invalidating else 1,
        "acceptsHWFocus": focus,
        "showWhenDataModelInvalid": "Hide",
        "whenVisible": "Always",
        "boundsType": "Absolute",
        "bounds": "%d %d %d %d" % (round(x), round(y), round(w), round(h)),
    }
    if invalidating:
        box["additionalInvalidatingHandles"] = invalidating
    return box


def entry(name, type_name, data, box, remap=None):
    return {
        "version": 2,
        "componentData": {"version": 1, "name": name, "type": type_name, "data": data},
        "handle remapping": {
            "version": 1,
            "map": [{"key": key, "value": value} for key, value in (remap or [])],
        },
        "bounds": box,
    }


def qlink_action():
    return {
        "version": 1, "onAction": "Mouse Down", "handler": "Q-Link",
        "handleName": "Data", "additionalData": "",
    }


def shell(children, w, h):
    return {
        "version": 2,
        "actions": [qlink_action()],
        "backgroundData": {
            "version": 1,
            "focussed": {"version": 1, "colour": "00000000", "image": ""},
            "unfocussed": {"version": 1, "colour": "00000000", "image": ""},
        },
        "ignoreMousePresses": False,
        "disableCoarseDataWheel": False,
        "componentsData": [
            entry("focus", "Focus", {
                "version": 1, "backgroundColour": "19ffffff", "outlineColour": "ff39d6ff",
                "backgroundInset": 4.0, "outlineThickness": 1.0,
            }, bounds(0, 0, w, h, "No"), )
        ] + children,
    }


def label_data(kind, height, handle="Data"):
    return {
        "version": 1,
        "textStyle": {
            "version": 1,
            "font": {"version": 1, "name": "Titillium Web", "style": "Regular", "height": float(height)},
            "colour": "ffe6f1ed",
            "justification": "horizontallyCentred verticallyCentred",
            "case": "Original",
        },
        "type": kind,
        "handleName": handle,
    }


class Skin:
    def __init__(self):
        self.defs = []
        self.used = set()
        self.images = set()

    def define(self, key, value):
        if key not in self.used:
            self.defs.append({"key": key, "value": value})
            self.used.add(key)

    def image(self, name):
        self.images.add(name)
        return name

    def place(self, page, key, index, x, y, w, h):
        page.append(entry(key, key, {"version": 1, "handleName": "Data"},
                           bounds(x, y, w, h), [("Data", param(index))]))

    def knob(self, page, index, x, y, w, h, radius):
        size = radius * 2 + 10
        frames = 128
        strip = self.image("rlo_knob_%d.png" % radius)
        key = "knob_%d_%d_%d" % (radius, w, h)
        kx = max(0, (w - size) // 2)
        ky = max(0, (h - size) // 2 - 10)
        self.define(key, shell([
            entry("knob", "Knob", {
                "version": 3, "knobType": "FilmStrip", "filmStrip": strip,
                "numFrames": frames, "invert": False, "dragOrientation": "Vertical",
                "handleName": "Data",
            }, bounds(kx, ky, size, size, "No")),
            entry("value", "Label", label_data("Value", 22),
                  bounds(4, h - 32, w - 8, 28, "No")),
        ], w, h))
        self.place(page, key, index, x, y, w, h)

    def toggle(self, page, index, x, y, w, h, stem):
        on = self.image("%s_on.png" % stem)
        off = self.image("%s_off.png" % stem)
        key = "btn_" + stem
        self.define(key, shell([
            entry("button", "Button", {
                "version": 2, "onImage": on, "offImage": off,
                "buttonId": 1, "numButtonsInGroup": 1,
                "handleName": "Data", "gestureBehaviour": "Instant",
            }, bounds(0, 0, w, h, "No")),
        ], w, h))
        self.place(page, key, index, x, y, w, h)

    def options(self, page, index, x, y, w, h, stem, count, ids=None):
        ids = list(range(count)) if ids is None else ids
        for option, button_id in enumerate(ids):
            name = "%s_%d_%d_%d" % (stem, option, w, h)
            on = self.image("%s_on.png" % name)
            off = self.image("%s_off.png" % name)
            key = "opt_" + name
            self.define(key, shell([
                entry("button", "Button", {
                    "version": 2, "onImage": on, "offImage": off,
                    "buttonId": button_id, "numButtonsInGroup": count,
                    "handleName": "Data", "gestureBehaviour": "Instant",
                }, bounds(0, 0, w, h, "No")),
            ], w, h))
            self.place(page, key, index, x + option * (w + 8), y, w, h)


def prepare_timing(src, dest):
    im = Image.open(src).convert("RGB")
    draw = ImageDraw.Draw(im)
    draw.rectangle((16, 234, 1264, 620), fill=(7, 12, 19))
    font = ImageFont.truetype(str(FONT), 28)
    ink = (159, 184, 196)
    for text, xy in (
        ("After recording", (40, 268)),
        ("Loop launch", (40, 390)),
        ("Align loop", (670, 500)),
    ):
        draw.text(xy, text, font=font, fill=ink)
    im.save(dest)


def page_shell(children, background):
    return {
        "version": 2,
        "actions": [],
        "backgroundData": {
            "version": 1,
            "focussed": {"version": 1, "colour": "ff070c13", "image": background},
            "unfocussed": {"version": 1, "colour": "ff070c13", "image": background},
        },
        "ignoreMousePresses": False,
        "disableCoarseDataWheel": False,
        "componentsData": children,
    }


def build():
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir()
    skin = Skin()
    looper, mixer, timing = [], [], []

    skin.options(looper, P["auto"], 390, 8, 136, 56, "rlo_segment_auto_start", 2)
    for option, label in enumerate(("Wave", "Tape", "Meters")):
        on = skin.image("rlo_segment_view_%d_106_56_on.png" % option)
        off = skin.image("rlo_segment_view_%d_106_56_off.png" % option)
        looper.append(entry(label, "Button", {
            "version": 2, "onImage": on, "offImage": off,
            "buttonId": option, "numButtonsInGroup": 3,
            "handleName": "Local/3/view", "gestureBehaviour": "Instant",
        }, bounds(690 + option * 114, 8, 106, 56)))
    for option in range(3):
        skin.image("rlo_view_%d.png" % option)
        looper.append(entry("view %d" % option, "Image", {
            "version": 2, "imageType": "Regular", "colour": "0",
            "image": "rlo_view_%d.png" % option,
        }, bounds(100, 78, 1080, 298, "No",
                  ["IndexedEnabling/%d/3/Local/3/view" % option])))
    skin.options(looper, P["length"], 1032, 8, 76, 56, "rlo_segment_length_ratio", 3)

    skin.toggle(looper, P["record"], 16, 384, 240, 88, "rlo_button_record_240_88")
    skin.toggle(looper, P["play"], 268, 384, 240, 88, "rlo_button_play_240_88")
    skin.toggle(looper, P["dub"], 520, 384, 240, 88, "rlo_button_dub_240_88")
    skin.toggle(looper, P["stop"], 772, 384, 240, 88, "rlo_button_stop_now_240_88")
    skin.toggle(looper, P["clear"], 1024, 384, 240, 88, "rlo_button_clear_240_88")

    skin.knob(looper, P["thresh"], 16, 492, 164, 124, 28)
    skin.toggle(looper, P["undo"], 192, 496, 120, 56, "rlo_button_undo_120_56")
    skin.toggle(looper, P["redo"], 320, 496, 120, 56, "rlo_button_redo_120_56")
    skin.toggle(looper, P["mute"], 192, 558, 120, 56, "rlo_button_mute_120_56")
    skin.toggle(looper, P["export"], 320, 558, 120, 56, "rlo_button_export_audio_120_56")
    skin.toggle(looper, P["reverse"], 448, 494, 120, 120, "rlo_button_reverse_120_120")
    skin.knob(looper, P["hp"], 584, 492, 220, 124, 28)
    skin.knob(looper, P["lp"], 812, 492, 220, 124, 28)
    skin.knob(looper, P["loop"], 1040, 492, 224, 124, 28)

    for column, key, radius in (
        (0, "input", 38), (1, "loop", 38), (2, "pan", 38),
        (3, "master", 38), (4, "feedback", 38), (5, "fade", 38),
    ):
        skin.knob(mixer, P[key], 24 + 208 * column, 84, 184, 170, radius)
    skin.options(mixer, P["source"], 36, 360, 138, 64, "rlo_segment_input", 4)
    # Output is on at value 1. The art's first segment says Channel and the second says Off.
    skin.options(mixer, P["output"], 666, 360, 138, 64, "rlo_segment_output", 2, ids=[1, 0])
    skin.knob(mixer, P["bars"], 200, 400, 160, 72, 22)
    skin.toggle(mixer, P["reverse"], 420, 408, 220, 56, "rlo_button_reverse_220_56")
    skin.toggle(mixer, P["export"], 960, 548, 280, 64, "rlo_button_export_audio_280_64")

    skin.options(timing, P["length"], 470, 140, 116, 64, "rlo_segment_length_ratio", 3)
    skin.options(timing, P["after"], 40, 300, 278, 56, "rlo_segment_after_record", 2)
    skin.options(timing, P["launch"], 40, 430, 260, 56, "rlo_segment_launch_mode", 2)
    skin.toggle(timing, P["sync"], 670, 530, 568, 56, "rlo_button_sync_start_568_56")

    for name in ("rlo_page_0.png", "rlo_page_1.png"):
        skin.image(name)
        shutil.copyfile(ART / name, OUT / name)
    prepare_timing(ART / "rlo_page_2.png", OUT / "rlo_page_2.png")
    skin.image("rlo_page_2.png")

    missing = []
    for name in sorted(skin.images):
        src = ART / name
        dst = OUT / name
        if name == "rlo_page_2.png":
            continue
        if not src.is_file():
            missing.append(name)
            continue
        if not dst.exists():
            shutil.copyfile(src, dst)
    if missing:
        raise SystemExit("missing art: " + ", ".join(missing))

    replacements = [{"key": param(i), "value": ["Off", "On"]} for i in BOOLS]
    replacements += [{"key": param(i), "value": labels} for i, labels in CHOICES.items()]
    tabs = []
    for index, (title, children, background) in enumerate((
        ("LOOPER", looper, "rlo_page_0.png"),
        ("MIXER", mixer, "rlo_page_1.png"),
        ("TIMING", timing, "rlo_page_2.png"),
    )):
        tabs.append({
            "version": 1,
            "tabName": title,
            "fnKeyIndex": index,
            "fnKeySubIndex": 0,
            "componentDefinition": page_shell(children, background),
            "qlinkBoundsData": ["16 8 1264 150", "16 160 1264 220", "16 384 1264 110", "16 490 1264 138"],
        })

    # Default bank, top to bottom on a 4-knob machine: Loop, High Pass, Low Pass, Input.
    looper_links = {
        13: P["loop"], 9: P["hp"], 5: P["lp"], 1: P["input"],
        14: P["thresh"], 10: P["auto"], 6: P["record"], 2: P["play"],
        15: P["dub"], 11: P["mute"], 7: P["length"], 3: P["after"],
        16: P["launch"], 12: P["bars"], 8: P["source"], 4: P["feedback"],
    }
    mixer_links = {
        13: P["input"], 9: P["loop"], 5: P["pan"], 1: P["master"],
        14: P["feedback"], 10: P["fade"], 6: P["source"], 2: P["output"],
        15: P["bars"], 11: P["reverse"], 7: P["export"], 3: P["hp"],
        16: P["lp"], 12: P["thresh"], 8: P["auto"], 4: P["sync"],
    }
    timing_links = {
        13: P["length"], 9: P["after"], 5: P["launch"], 1: P["sync"],
        14: P["bars"], 10: P["follow"], 6: P["play"], 2: P["record"],
        15: P["dub"], 11: P["stop"], 7: P["mute"], 3: P["clear"],
        16: P["undo"], 12: P["redo"], 8: P["reverse"], 4: P["export"],
    }

    def qmap(links):
        return {"Q-Link %d" % n: links.get(n, -1) for n in range(1, 17)}

    screen = []
    for tab, links in enumerate((looper_links, mixer_links, timing_links), start=1):
        screen.append({
            "Tab": tab, "SubTab": 1, "Bank Direction": "Column", "Q-Links": qmap(links),
        })
    tui = {
        "pageData": {
            "version": 1,
            "componentDefinitions": {
                "version": 2, "importFiles": [],
                "localComponentDefinitions": skin.defs,
            },
            "replacementStrings": replacements,
            "info": {"version": 1, "type": "CompleteDescription"},
            "tabs": tabs,
        }
    }
    qlinks = {
        "version": 4,
        "info": {"version": 1, "type": "CompleteDescription"},
        "Screen Mode Q-Links": {"version": 4, "map": screen},
        "Program Mode Q-Links": qmap(looper_links),
    }
    import json
    (OUT / "TUI.json").write_text(json.dumps(tui, indent=2) + "\n")
    (OUT / "Q-Links.json").write_text(json.dumps(qlinks, indent=2) + "\n")
    print("wrote %s (%d images, %d components)" % (OUT, len(skin.images), len(skin.defs)))


if __name__ == "__main__":
    os.environ["COPYFILE_DISABLE"] = "1"
    build()
