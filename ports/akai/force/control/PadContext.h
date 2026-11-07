// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
/* Force's Matrix/Arrange remain fully native when the editor is closed.
   The 16 looper controls occupy the lower two rows only while its editor is
   visible; native mute, sequencing, MIDI and pad-performance modes yield. */
static inline int rlo_pad_context_allowed(uint32_t context){
 unsigned mode=context>>16,popup=context&0xffff;
 if(popup!=0x30fa)return 0;
 return mode==0x2080||mode==0x2083||mode==0x2084||mode==0x2089||mode==0x208b||mode==0x209c;
}
