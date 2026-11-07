// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
/* Passive MIDI parser: running status, split reads, realtime and SysEx safe. */
typedef struct {uint8_t status,data[2],used,needed,sysex,rec_down,dub_down;uint32_t capture_presses;} RloButtonParser;
static inline unsigned rlo_button_bytes(RloButtonParser* p,const unsigned char* bytes,size_t count){
    unsigned presses=0;
    for(size_t i=0;i<count;++i){unsigned b=bytes[i];
        if(b>=0xf8)continue;
        if(b==0xf0){p->sysex=1;p->status=p->used=0;continue;}
        if(b==0xf7){p->sysex=0;p->status=p->used=0;continue;}
        if(p->sysex)continue;
        if(b&0x80){p->used=0;p->status=b;p->needed=(b>=0xf0)?0:((b&0xe0)==0xc0?1:2);continue;}
        if(!p->status||!p->needed)continue;
        p->data[p->used++]=b;if(p->used<p->needed)continue;p->used=0;
        if((p->status==0x90||p->status==0x80)&&p->data[0]==73){
            unsigned down=p->status==0x90&&p->data[1]>0;
            if(down&&!p->rec_down){++presses;++p->capture_presses;}p->rec_down=down;
        }
        if((p->status==0x90||p->status==0x80)&&p->data[0]==80){
            unsigned down=p->status==0x90&&p->data[1]>0;
            if(down&&!p->dub_down)++p->capture_presses;p->dub_down=down;
        }
    }
    return presses;
}
