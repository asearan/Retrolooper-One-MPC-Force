#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
/* Independent Force implementation using controller protocol facts: pads
   are channel 10 notes 0x36..0x75, row-major from the upper-left corner. */
typedef struct {uint8_t status,data[2],used,needed,sysex,shift,bank,known,seen_note,last_note,last_pad,down[128],picked[64];} RloPadParser;
typedef int (*RloPadSink)(void*,unsigned,int);
static inline int rlo_force_slot(unsigned physical){
 return physical>=56&&physical<64?(int)(physical-56):physical>=48&&physical<56?(int)(physical-48+8):-1;
}
static inline int rlo_pad_number(unsigned note){return note>=0x36&&note<=0x75?rlo_force_slot(note-0x36):-1;}
static inline void rlo_pad_input(RloPadParser *p,unsigned char *bytes,size_t size,RloPadSink sink,void *context,RloPadSink picker){
 p->known=1;p->bank=0;
 for(size_t i=0;i<size;++i){unsigned b=bytes[i];if(b>=0xf8)continue;
  if(b==0xf0){p->sysex=1;p->status=p->used=0;continue;}if(b==0xf7){p->sysex=0;p->status=p->used=0;continue;}if(p->sysex)continue;
  if(b&0x80){p->status=b;p->used=0;p->needed=b>=0xf0?0:(b&0xe0)==0xc0?1:2;continue;}
  if(!p->status||!p->needed)continue;p->data[p->used++]=b;if(p->used<p->needed)continue;p->used=0;
  if(p->status!=0x99&&p->status!=0x89&&p->status!=0xa9)continue;
  unsigned note=p->data[0],value=p->data[1];int slot=rlo_pad_number(note),attack=p->status==0x99&&value>0,press=attack&&!p->down[note];
  p->seen_note=1;p->last_note=(uint8_t)note;p->last_pad=slot<0?255:(uint8_t)slot;
  if(p->status!=0xa9)p->down[note]=(uint8_t)attack;
  if(note>=0x36&&note<=0x75){
   unsigned pad=note-0x36;
   if(press&&picker&&picker(context,pad,1))p->picked[pad]=1;
   if(p->picked[pad]){
    if(p->status==0xa9||attack)bytes[i]=0;
    else p->picked[pad]=0;
    continue;
   }
  }
  if(slot>=0&&sink&&sink(context,(unsigned)slot,press)&&(p->status==0x99||p->status==0xa9))bytes[i]=0;
 }
}
typedef struct {uint8_t bytes[16][12],seen[16],packet[12],used,overflow;uint32_t dirty;} RloPadColours;
static inline void rlo_pad_native_colours(RloPadColours *p,const unsigned char *bytes,size_t size){
 for(size_t i=0;i<size;++i){unsigned b=bytes[i];if(b>=0xf8)continue;if(b==0xf0){p->used=1;p->overflow=0;p->packet[0]=b;continue;}if(!p->used)continue;
  if(p->used<12)p->packet[p->used++]=b;else p->overflow=1;if(b!=0xf7)continue;const uint8_t *s=p->packet;
  if(!p->overflow&&p->used==12&&s[1]==0x47&&s[2]<128&&s[3]<128&&s[4]==0x65&&s[5]==0&&s[6]==4&&s[7]<64&&s[8]<128&&s[9]<128&&s[10]<128){int slot=rlo_force_slot(s[7]);if(slot>=0){memcpy(p->bytes[slot],s,12);p->seen[slot]=1;p->dirty|=1u<<slot;}}
  p->used=p->overflow=0;
 }
}
static inline int rlo_pad_colours_ready(const RloPadColours *p){for(unsigned i=0;i<16;++i)if(!p->seen[i])return 0;return 1;}
static inline void rlo_pad_colour_packet(const RloPadColours *p,unsigned slot,const uint8_t rgb[3],uint8_t out[12]){memcpy(out,p->bytes[slot],12);memcpy(out+8,rgb,3);}
