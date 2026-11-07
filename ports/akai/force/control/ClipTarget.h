#pragma once
#include "ClipProtocol.h"

/* Force 3.10: Project's native ClipCoordinates and the bounded row lookup
   used by the clip browser (0x28f17f0). All model reads use native_read. */
static uint32_t force_engine(void){
 uint32_t window=native_word(native_application+0xc4);
 if(!word_is(window,native_base+0x6cdeeb4))return 0;
 uint32_t engine=native_word(window+0x26c);
 return word_is(engine,native_base+0x6ce4670)&&word_is(engine+0x264,native_project)?engine:0;
}
static int force_clip_slot(uint32_t lanes,uint32_t track,uint32_t row,uint32_t *clip,uint32_t *owner){
 if(!lanes)return 0;
 uint32_t begin=native_word(lanes+0x60),end=native_word(lanes+0x64);
 if(!begin||end<begin||(end-begin)%136||end-begin>4096u*136u||!row||row>=(end-begin)/136)return 0;
 uint32_t root;
 if(!native_read(&root,begin+136u*row+0x78,4))return 0;
 *clip=*owner=0;
 for(unsigned depth=0;root&&depth<32;++depth){
  uint32_t node[7];
  if(!native_read(node,root,sizeof node))return 0;
  if(node[4]==track){*clip=node[5];*owner=node[6];return 1;}
  root=node[node[4]<track?3:2];
 }
 return !root;
}
static int force_clip_target(uint32_t track,RloForceClipTarget *out){
 unsigned char arrangement=1;
 uint32_t row=native_word(native_project+0x1b8),lanes=native_word(native_project+0xb20),clip,owner;
 if(!track||!word_is(native_project+0xa0,track)||!word_is(native_project+0x1b4,track)
    ||!native_read(&arrangement,native_project+0x1bc,1)||arrangement
    ||!force_clip_slot(lanes,track,row,&clip,&owner))return 0;
 *out=(RloForceClipTarget){.project=(uint32_t)native_project,.track=track,.row=row,.clip=clip,.lanes=lanes,.owner=owner};
 return 1;
}
