// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
/* MPC private-controller facts, independent of the plugin's MIDI note map.
   Keep every status byte and length intact: owned pad attacks become velocity
   zero releases, so running status and split reads remain valid for MPC. */
typedef struct {uint8_t status,data[2],used,needed,sysex,shift,bank,known,seen_note,last_note,last_pad,down[128];} RloPadParser;
typedef int (*RloPadSink)(void*,unsigned,int);
static inline int rlo_pad_number(unsigned note){
 /* Native MPC private pad identities, bottom-left Pad 1 through Pad 16.
    Public/program MIDI notes are a separate, user-editable mapping. */
 static const uint8_t native_notes[16]={37,36,42,82,40,38,46,44,48,47,45,43,49,55,51,53};
 for(unsigned i=0;i<16;++i)if(native_notes[i]==note)return (int)i;
 return note<16?(int)note:-1;
}
static inline void rlo_pad_input(RloPadParser *p,unsigned char *bytes,size_t size,RloPadSink sink,void *context){
 for(size_t i=0;i<size;++i){unsigned b=bytes[i];
  if(b>=0xf8)continue;
  if(b==0xf0){p->sysex=1;p->status=p->used=0;continue;}
  if(b==0xf7){p->sysex=0;p->status=p->used=0;continue;}
  if(p->sysex)continue;
  if(b&0x80){p->status=b;p->used=0;p->needed=b>=0xf0?0:(b&0xe0)==0xc0?1:2;continue;}
  if(!p->status||!p->needed)continue;
  p->data[p->used++]=b;if(p->used<p->needed)continue;p->used=0;
  unsigned note=p->data[0],value=p->data[1];
  if(p->status==0x90||p->status==0x80){
   int down=p->status==0x90&&value>0;
   if(note==49)p->shift=(uint8_t)down;
   if(down&&note>=35&&note<=38){p->bank=(uint8_t)(note-35+(p->shift?4:0));p->known=1;memset(p->down,0,sizeof p->down);}
  }
  if(p->status!=0x99&&p->status!=0x89&&p->status!=0xa9)continue;
  int pad=rlo_pad_number(note),attack=p->status==0x99&&value>0;
  p->seen_note=1;p->last_note=(uint8_t)note;p->last_pad=pad<0?255:(uint8_t)pad;
  int press=attack&&!p->down[note];
  if(p->status!=0xa9)p->down[note]=(uint8_t)attack;
  if(pad>=0&&p->known&&p->bank==0&&sink&&sink(context,(unsigned)pad,press)){
   if(p->status==0x99||p->status==0xa9)bytes[i]=0;
  }
 }
}

typedef struct {uint8_t bytes[16][12],seen[16],packet[12],used,overflow;uint32_t dirty;} RloPadColours;
static inline void rlo_pad_native_colours(RloPadColours *p,const unsigned char *bytes,size_t size){
 for(size_t i=0;i<size;++i){unsigned b=bytes[i];if(b>=0xf8)continue;
  if(b==0xf0){p->used=1;p->overflow=0;p->packet[0]=(uint8_t)b;continue;}
  if(!p->used)continue;
  if(p->used<12)p->packet[p->used++]=(uint8_t)b;else p->overflow=1;
  if(b!=0xf7)continue;
  const uint8_t *s=p->packet;
  if(!p->overflow&&p->used==12&&s[1]==0x47&&s[2]<128&&s[3]<128&&s[4]==0x65&&s[5]==0&&s[6]==4&&s[7]<16&&s[8]<128&&s[9]<128&&s[10]<128){
   unsigned pad=s[7];memcpy(p->bytes[pad],s,12);p->seen[pad]=1;p->dirty|=1u<<pad;
  }
  p->used=p->overflow=0;
 }
}
static inline int rlo_pad_colours_ready(const RloPadColours *p){for(unsigned i=0;i<16;++i)if(!p->seen[i])return 0;return 1;}
static inline void rlo_pad_colour_packet(const RloPadColours *p,unsigned pad,const uint8_t rgb[3],uint8_t out[12]){
 memcpy(out,p->bytes[pad],12);memcpy(out+8,rgb,3);
}
