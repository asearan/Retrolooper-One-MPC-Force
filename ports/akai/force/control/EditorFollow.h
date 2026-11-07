#pragma once
#include "PadContext.h"
/* Force-only, UI-thread editor following. Opening an editor selects its owner;
   a subsequent native track change retargets that editor through Effects' own
   session-opening method. Never assign session/shared_ptr fields directly. */
static uint32_t force_last_selected,force_follow_target,force_follow_effect,force_follow_started;
static uint32_t force_editor_switches,force_editor_closes;
static RloEditorOwner force_clip_editor;
static uint32_t force_clip_editor_selection;
#ifdef RLO_TRACK_BRIDGE_TEST
static void (*test_force_editor_open)(uint32_t,uint32_t,uint32_t,uint32_t);
static void (*test_force_editor_close)(uint32_t,unsigned,unsigned);
#endif
static int force_editor_switch_fingerprint(void){
#if defined(RLO_TRACK_BRIDGE_TEST) && !defined(RLO_EDITOR_READONLY_PROBE)
 return 1;
#else
 return word_is(native_base+0x29eeaec,0xe16d42f0)&&word_is(native_base+0x29eeaf0,0xe5d0c03c)
  &&word_is(native_base+0x29eeb88,0xe59705a4)&&word_is(native_base+0x29eedb8,0xebfffe65)
  &&word_is(native_base+0x38d0360,0xe59031f8)&&word_is(native_base+0x38d0364,0xe59001fc)
  &&word_is(native_base+0x38d0368,0xebc479df)&&word_is(native_base+0x165b5a8,0xe16d42f0)
  &&word_is(native_base+0x165b5d4,0xe3520dc2)&&word_is(native_base+0x6cb6dfc,native_base+0x29eda30);
#endif
}
/* Returns 1 for a uniquely owned looper, 2 for an owned track without one,
   zero for malformed/ambiguous or concurrently changed native state. */
static int force_editor_target(uint32_t selected,unsigned preferred,uint32_t *program,unsigned *slot,uint32_t *effect){
 *program=*effect=0;*slot=0;
 if(!word_is(native_project,native_base+0x6d4f1e4))return 0;
 uint32_t pool=native_word(native_project+0xb1c);
 if(!word_is(pool,native_base+0x6cb6548))return 0;
 uint32_t begin=native_word(pool+0x60),end=native_word(pool+0x64);
 if(end<begin||(end-begin)%4||end-begin>MAX_NATIVE_TRACKS*4)return 0;
 unsigned owned=0;for(uint32_t at=begin;at<end;at+=4)if(native_word(at)==selected)++owned;
 if(!selected)return native_word(native_project+0xa0)==0?2:0;
 if(owned!=1)return 0;
 uint32_t arm=0,fx[MAX_TRACK_FX];unsigned count=0;
 if(!track_effects(selected,program,&arm,fx,&count)||native_word(native_project+0xa0)!=selected)return 0;
 if(!count)return 2;
 for(uint32_t at=begin;at<end;at+=4){uint32_t other=native_word(at);
  if(other==selected)continue;
  if(!word_is(other,native_base+0x6d50644))return 0;
  if(native_word(other+0x64c)==6&&native_word(other+0x648)==*program)return 0;
 }
 if(!word_is(*program+0x3c,native_base+0x6d4b81c))return 0;
 uint32_t chain=native_word(*program+0x5e0),first=native_word(chain+0x60),last=native_word(chain+0x64);
 unsigned available=(last-first)/4;uint32_t wanted=fx[0];
 if(preferred<available){uint32_t insert=native_word(first+preferred*4),wrapper=native_word(insert+0x68),candidate=native_word(wrapper+0xcc);
  for(unsigned i=0;i<count;++i)if(fx[i]==candidate)wanted=candidate;
 }
 for(unsigned i=0;i<available;++i){uint32_t insert=native_word(first+i*4),wrapper=native_word(insert+0x68);
  if(native_word(wrapper+0xcc)==wanted){*slot=i;*effect=wanted;return 1;}
 }
 return 0;
}
static int force_editor_retarget(const RloEditorOwner *current,uint32_t selected){
 if(!force_editor_switch_fingerprint()||native_word(current->component+0x1f8)!=native_engine_mode)return 0;
 uint32_t manager=native_word(current->component+0x1fc);
 if(!word_is(manager,native_base+0x6cb6dfc)||native_word(manager+0xc)!=native_queues||native_word(manager+0x18)!=current->session)return 0;
 uint32_t program=0,effect=0;unsigned slot=0;
 int result=force_editor_target(selected,native_word(current->session+0x38),&program,&slot,&effect);
 if(!result)return 0;
 if(result==2){
#ifdef RLO_TRACK_BRIDGE_TEST
  if(!test_force_editor_close)return 0;test_force_editor_close(native_engine_mode,0,0x30fa);
#else
  ((void(*)(void*,unsigned,unsigned))(native_base+0x165b5a8))((void*)native_engine_mode,0,0x30fa);
#endif
  ++force_editor_closes;force_follow_target=force_follow_effect=0;
  memset(&last_editor_owner,0,sizeof last_editor_owner);visible_editor_fx=0;
  visible_editor_open=native_word(native_engine_mode+0x6c)==0x30fa;return 1;
 }
 force_follow_target=selected;force_follow_effect=effect;force_follow_started=now_ms();visible_editor_fx=0;
#ifdef RLO_TRACK_BRIDGE_TEST
 if(!test_force_editor_open){force_follow_target=force_follow_effect=0;return 0;}
 test_force_editor_open(manager,program+0x3c,slot,native_engine_mode);
#else
 ((void(*)(void*,void*,unsigned,void*))(native_base+0x29eeaec))((void*)manager,(void*)(program+0x3c),slot,(void*)native_engine_mode);
#endif
 ++force_editor_switches;return 1;
}
static void force_follow_editor_track(void){
 RloEditorOwner current;uint32_t selected=native_word(native_project+0xa0);
 visible_editor_fx=0;visible_editor_open=native_word(native_engine_mode+0x6c)==0x30fa;
 uint32_t context=(native_word(native_engine_mode+0x40)<<16)|(native_word(native_engine_mode+0x6c)&0xffff);
 if(!rlo_pad_context_allowed(context)){memset(&last_editor_owner,0,sizeof last_editor_owner);force_clip_editor_selection=0;force_follow_target=force_follow_effect=0;force_last_selected=selected;return;}
 if(!editor_owner(&current)){
  force_clip_editor_selection=0;
  if(!visible_editor_open||!force_follow_target){memset(&last_editor_owner,0,sizeof last_editor_owner);force_follow_target=force_follow_effect=0;}
  force_last_selected=selected;return;
 }
 if(force_clip_editor_selection){
  if(selected==force_clip_editor_selection&&!memcmp(&current,&force_clip_editor,sizeof current)){
   last_editor_owner=current;visible_editor_fx=current.effect;
   force_follow_target=force_follow_effect=0;force_last_selected=selected;
   return;
  }
  force_clip_editor_selection=0;
 }
 int had_editor=last_editor_owner.component!=0;
 if(had_editor&&selected!=force_last_selected&&(selected!=current.track||force_follow_target)){
  force_editor_retarget(&current,selected);force_last_selected=selected;
  if(!editor_owner(&current)||current.track!=selected)return;
 }
 if(force_follow_target){
  if(current.track==force_follow_target&&current.effect==force_follow_effect){force_follow_target=force_follow_effect=0;}
  else {if(now_ms()-force_follow_started>750)force_follow_target=force_follow_effect=0;force_last_selected=selected;return;}
 }
 if(had_editor&&selected!=current.track&&!memcmp(&current,&last_editor_owner,sizeof current)){
  /* A failed/unsupported retarget cannot select the old track again or
     give stale editor pads permission to control it. */
  force_last_selected=selected;return;
 }
 follow_editor_track();force_last_selected=native_word(native_project+0xa0);
}
