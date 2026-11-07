# Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
# SPDX-License-Identifier: GPL-3.0-only
"""A preload library must never export compiler/runtime helpers into MPC."""
from pathlib import Path
import struct
p=Path(__file__).with_name('retrolooper_buttons.so');d=p.read_bytes()
assert d[:6]==b'\x7fELF\x01\x01'
start=struct.unpack_from('<I',d,32)[0];step,count=struct.unpack_from('<HH',d,46)
sections=[struct.unpack_from('<10I',d,start+i*step) for i in range(count)];exports=set()
for s in sections:
    if s[1]!=11:continue
    names=sections[s[6]];strings=d[names[4]:names[4]+names[5]]
    for at in range(s[4],s[4]+s[5],s[9]):
        name,value,size,info,other,index=struct.unpack_from('<IIIBBH',d,at)
        if index and info>>4 in (1,2) and other&3==0:exports.add(strings[name:].split(b'\0')[0].decode())
expected={'snd_rawmidi_open','snd_rawmidi_read','snd_rawmidi_close','snd_rawmidi_write','retrolooper_mpc_rec_generation','retrolooper_mpc_button_ready','retrolooper_mpc_track_status','retrolooper_mpc_track_prepare','retrolooper_mpc_pad_take','retrolooper_mpc_pad_publish'}
assert exports==expected,sorted(exports-expected)
print('PASS: companion exports only its ten controller/track/pad functions; all compiler/runtime symbols hidden')
