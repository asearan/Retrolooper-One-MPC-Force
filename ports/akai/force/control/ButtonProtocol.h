#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct {uint8_t status,data[2],used,needed,sysex,rec_down,dub_down;uint32_t capture_presses;} RloButtonParser;
/* Force's physical buttons use CC on the private controller port. REC is
   control 0x49; MPC's channel-1 note protocol is not used here. */
static inline unsigned rlo_button_bytes(RloButtonParser *p,const unsigned char *bytes,size_t size){
 unsigned presses=0;
 for(size_t i=0;i<size;++i){unsigned b=bytes[i];if(b>=0xf8)continue;
  if(b==0xf0){p->sysex=1;p->status=p->used=0;continue;}if(b==0xf7){p->sysex=0;p->status=p->used=0;continue;}if(p->sysex)continue;
  if(b&0x80){p->status=b;p->used=0;p->needed=b>=0xf0?0:(b&0xe0)==0xc0?1:2;continue;}
  if(!p->needed||!p->status)continue;p->data[p->used++]=b;if(p->used<p->needed)continue;p->used=0;
  if(p->status==0xb0&&p->data[0]==0x49){unsigned down=p->data[1]>0;if(down&&!p->rec_down){++presses;++p->capture_presses;}p->rec_down=down;}
 }return presses;
}
