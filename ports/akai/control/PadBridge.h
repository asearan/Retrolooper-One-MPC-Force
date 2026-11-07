// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PadProtocol.h"
#include "TrackProtocol.h"
#include "PadContext.h"
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
enum { RLO_PAD_PRESENT=1,RLO_PAD_BANK_A=2,RLO_PAD_LEDS_READY=4,RLO_PAD_ACTIVE=8 };
typedef struct {uint32_t effect,token,stamp,enabled;uint8_t rgb[48];} RloPadFeedback;
typedef struct {uint32_t effect,token,stamp,pad,context_epoch;} RloPadEvent;
static RloPadParser pad_parser;
static RloPadColours pad_native;
static RloPadFeedback pad_feedback[2];
static uint32_t pad_slot,pad_readers[2],pad_writer;
static RloPadEvent pad_events[64];
static uint32_t pad_head,pad_tail,pad_taker;
static int pad_bank=-1;
static int pad_last_note=-1,pad_last_number=-1;
static snd_rawmidi_t *pad_output;
typedef struct {snd_rawmidi_t *handle;RloPadColours colours;} RloPadOutput;
static RloPadOutput pad_outputs[4];
static void pad_default_bank(void){pad_parser.known=1;pad_parser.bank=0;__atomic_store_n(&pad_bank,0,__ATOMIC_RELEASE);}
static pthread_mutex_t pad_output_gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t pad_thread_once=PTHREAD_ONCE_INIT;
static uint8_t pad_sent[48];
static uint32_t pad_lit,pad_led_ready;
static uint32_t pad_context,pad_context_stamp,pad_context_epoch;
static uint32_t pad_now(void){
#ifdef RLO_PAD_BRIDGE_TEST
 return test_pad_now();
#else
 struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint32_t)((uint64_t)t.tv_sec*1000+t.tv_nsec/1000000);
#endif
}
/* File reads stay on MIDI/LED workers. Audio callbacks only read cached
   atomics. Polling native state also covers touchscreen mode changes. */
static void pad_refresh_context(void){
 uint32_t context=0;
#ifdef RLO_PAD_BRIDGE_TEST
 context=test_pad_context();
#elif defined(__arm__) && UINTPTR_MAX==0xffffffffu
 if(word_is(native_engine_mode,native_base+0x6ca3344)){
  uint32_t mode=native_word(native_engine_mode+0x40),popup=native_word(native_engine_mode+0x6c);
  if(mode<=0xffff&&popup<=0xffff&&mode==native_word(native_engine_mode+0x40))context=(mode<<16)|popup;
 }
#endif
 if(__atomic_exchange_n(&pad_context,context,__ATOMIC_ACQ_REL)!=context)__atomic_fetch_add(&pad_context_epoch,1,__ATOMIC_ACQ_REL);
 __atomic_store_n(&pad_context_stamp,pad_now(),__ATOMIC_RELEASE);
}
static int pad_context_ready(void){
 return pad_now()-__atomic_load_n(&pad_context_stamp,__ATOMIC_ACQUIRE)<100
   &&rlo_pad_context_allowed(__atomic_load_n(&pad_context,__ATOMIC_ACQUIRE));
}
static int pad_owner(void *effect){
 if(!pad_context_ready())return 0;
#ifdef RLO_PAD_BRIDGE_TEST
 return test_pad_owner(effect);
#elif defined(__arm__) && UINTPTR_MAX==0xffffffffu
 RloTrackSnapshot view;
 return snapshot_read(&view)&&view.count&&(view.editor_open?view.editor_fx:view.fx[0])==(uintptr_t)effect
   &&(retrolooper_mpc_track_status(effect)&(RLO_TRACK_READY|RLO_TRACK_SELECTED|RLO_TRACK_MONITOR_ROUTED))==(RLO_TRACK_READY|RLO_TRACK_SELECTED|RLO_TRACK_MONITOR_ROUTED);
#else
 (void)effect;return 0;
#endif
}
static int pad_feedback_read(RloPadFeedback *out){
 for(int attempt=0;attempt<2;++attempt){
  unsigned slot=__atomic_load_n(&pad_slot,__ATOMIC_ACQUIRE);__atomic_fetch_add(&pad_readers[slot],1,__ATOMIC_ACQ_REL);
  if(slot!=__atomic_load_n(&pad_slot,__ATOMIC_ACQUIRE)){__atomic_fetch_sub(&pad_readers[slot],1,__ATOMIC_RELEASE);continue;}
  *out=pad_feedback[slot];__atomic_fetch_sub(&pad_readers[slot],1,__ATOMIC_RELEASE);
  return out->effect&&(out->enabled&1)&&pad_now()-out->stamp<500&&pad_owner((void*)(uintptr_t)out->effect);
 }
 return 0;
}
static uint32_t pad_flags(void){return RLO_PAD_PRESENT|(__atomic_load_n(&pad_bank,__ATOMIC_ACQUIRE)==0?RLO_PAD_BANK_A:0)|(__atomic_load_n(&pad_led_ready,__ATOMIC_ACQUIRE)?RLO_PAD_LEDS_READY:0);}
__attribute__((visibility("default"))) uint32_t retrolooper_mpc_pad_publish(void *effect,uint32_t token,uint32_t enabled,const uint8_t *rgb){
 uint32_t flags=pad_flags();if(!effect||!token||!rgb||!pad_owner(effect))return flags;
 uint32_t expected=0;if(!__atomic_compare_exchange_n(&pad_writer,&expected,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return flags;
 unsigned slot=__atomic_load_n(&pad_slot,__ATOMIC_RELAXED)^1;
 if(!__atomic_load_n(&pad_readers[slot],__ATOMIC_ACQUIRE)){
  RloPadFeedback *next=&pad_feedback[slot];next->effect=(uint32_t)(uintptr_t)effect;next->token=token;next->stamp=pad_now();next->enabled=enabled;
  for(unsigned i=0;i<48;++i)next->rgb[i]=rgb[i]>127?127:rgb[i];
  __atomic_store_n(&pad_slot,slot,__ATOMIC_RELEASE);
 }
 __atomic_store_n(&pad_writer,0,__ATOMIC_RELEASE);
 return flags|((enabled&1)&&(flags&RLO_PAD_BANK_A)?RLO_PAD_ACTIVE:0);
}
static int pad_input_sink(void *context,unsigned pad,int press){
 (void)context;RloPadFeedback feedback;if(!pad_feedback_read(&feedback))return 0;
 if(press){
  if((feedback.enabled>>16)&(1u<<pad)){
#ifdef RLO_PAD_BRIDGE_TEST
   test_pad_prepare((void*)(uintptr_t)feedback.effect);
#else
   void *effect=(void*)(uintptr_t)feedback.effect;retrolooper_mpc_track_prepare(effect);
   uint32_t began=pad_now(),wanted=RLO_TRACK_READY|RLO_TRACK_SELECTED|RLO_TRACK_DISARMED|RLO_TRACK_MONITOR_ROUTED|RLO_TRACK_INPUT_READY;
   while(pad_now()-began<80&&(retrolooper_mpc_track_status(effect)&wanted)!=wanted){struct timespec pause={0,1000000};nanosleep(&pause,0);}
#endif
  }
  unsigned tail=__atomic_load_n(&pad_tail,__ATOMIC_RELAXED),head=__atomic_load_n(&pad_head,__ATOMIC_ACQUIRE);
  if(tail-head<64){pad_events[tail%64]=(RloPadEvent){feedback.effect,feedback.token,pad_now(),pad,__atomic_load_n(&pad_context_epoch,__ATOMIC_ACQUIRE)};__atomic_store_n(&pad_tail,tail+1,__ATOMIC_RELEASE);}
 }
 return 1;
}
static void pad_input_bytes(unsigned char *bytes,size_t size){
 pad_refresh_context();
 rlo_pad_input(&pad_parser,bytes,size,pad_input_sink,0);
 __atomic_store_n(&pad_bank,pad_parser.known?(int)pad_parser.bank:-1,__ATOMIC_RELEASE);
 if(pad_parser.seen_note){__atomic_store_n(&pad_last_note,pad_parser.last_note,__ATOMIC_RELEASE);__atomic_store_n(&pad_last_number,pad_parser.last_pad==255?-1:pad_parser.last_pad+1,__ATOMIC_RELEASE);}
}
__attribute__((visibility("default"))) int retrolooper_mpc_pad_take(void *effect,uint32_t token,uint32_t *pad){
 if(!pad||!pad_owner(effect))return 0;
 unsigned expected=0;if(!__atomic_compare_exchange_n(&pad_taker,&expected,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 0;
 int found=0;RloPadFeedback feedback;int ready=__atomic_load_n(&pad_bank,__ATOMIC_ACQUIRE)==0&&pad_feedback_read(&feedback);
 for(unsigned i=0;i<64;++i){
  unsigned head=__atomic_load_n(&pad_head,__ATOMIC_RELAXED),tail=__atomic_load_n(&pad_tail,__ATOMIC_ACQUIRE);if(head==tail)break;
  RloPadEvent event=pad_events[head%64];__atomic_store_n(&pad_head,head+1,__ATOMIC_RELEASE);
  if(ready&&event.effect==(uintptr_t)effect&&event.token==token&&feedback.token==token&&pad_now()-event.stamp<250&&event.pad<16&&event.context_epoch==__atomic_load_n(&pad_context_epoch,__ATOMIC_ACQUIRE)){*pad=event.pad;found=1;break;}
 }
 __atomic_store_n(&pad_taker,0,__ATOMIC_RELEASE);return found;
}
static int pad_write_packet(snd_rawmidi_t *output,const uint8_t *packet){
 size_t at=0;while(at<12){ssize_t n=next_write?next_write(output,packet+at,12-at):-ENOSYS;if(n<=0||n>(ssize_t)(12-at))return 0;at+=(size_t)n;}return 1;
}
static void pad_restore_locked(void){
 if(pad_output&&pad_lit)for(unsigned i=0;i<16;++i)if(pad_native.seen[i])pad_write_packet(pad_output,pad_native.bytes[i]);
 pad_lit=0;
}
static void pad_led_tick(void){
 pad_refresh_context();
 RloPadFeedback feedback;int active=__atomic_load_n(&pad_bank,__ATOMIC_ACQUIRE)==0&&pad_feedback_read(&feedback);
 pthread_mutex_lock(&pad_output_gate);
 if(!active||!pad_output||!rlo_pad_colours_ready(&pad_native)){pad_restore_locked();pthread_mutex_unlock(&pad_output_gate);return;}
 for(unsigned pad=0;pad<16;++pad){const uint8_t *rgb=feedback.rgb+pad*3;
  if(!pad_lit||(pad_native.dirty&(1u<<pad))||memcmp(pad_sent+pad*3,rgb,3)){
   uint8_t packet[12];rlo_pad_colour_packet(&pad_native,pad,rgb,packet);
   if(pad_write_packet(pad_output,packet)){memcpy(pad_sent+pad*3,rgb,3);pad_native.dirty&=~(1u<<pad);}
  }
 }
 pad_lit=1;pthread_mutex_unlock(&pad_output_gate);
}
static void *pad_led_worker(void *unused){
 (void)unused;int previous_bank=-2,previous_note=-2,previous_led=-1;uint32_t previous_owner=~0u,previous_context=~0u;
 for(;;){pad_led_tick();
  RloPadFeedback f;uint32_t owner=pad_feedback_read(&f)?f.effect:0;
  int bank=__atomic_load_n(&pad_bank,__ATOMIC_ACQUIRE),note=__atomic_load_n(&pad_last_note,__ATOMIC_ACQUIRE),led=__atomic_load_n(&pad_led_ready,__ATOMIC_ACQUIRE);
  uint32_t context=__atomic_load_n(&pad_context,__ATOMIC_ACQUIRE);
  if(bank!=previous_bank||note!=previous_note||led!=previous_led||owner!=previous_owner||context!=previous_context){
   int fd=open("/tmp/retrolooper-pad-state.json",O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
   if(fd>=0){dprintf(fd,"{\"bank\":%d,\"raw_note\":%d,\"pad\":%d,\"owner\":%u,\"led_ready\":%d,\"native_mode\":%u,\"native_popup\":%u,\"looper_context\":%d}\n",bank,note,__atomic_load_n(&pad_last_number,__ATOMIC_ACQUIRE),owner,led,context>>16,context&0xffff,rlo_pad_context_allowed(context));close(fd);}
   previous_bank=bank;previous_note=note;previous_led=led;previous_owner=owner;previous_context=context;
  }
  struct timespec pause={0,20000000};nanosleep(&pause,0);
 }return 0;
}
static void pad_start_thread(void){pthread_t thread;if(pthread_create(&thread,0,pad_led_worker,0)==0)pthread_detach(thread);}
static void pad_maybe_start(void){
#if defined(__arm__) && UINTPTR_MAX==0xffffffffu && !defined(RLO_PAD_BRIDGE_TEST)
 if(__atomic_load_n(&native_supported,__ATOMIC_ACQUIRE))pthread_once(&pad_thread_once,pad_start_thread);
#endif
}
static void pad_open_output(snd_rawmidi_t *output){
 pthread_mutex_lock(&pad_output_gate);
 for(unsigned i=0;i<4;++i){if(pad_outputs[i].handle==output)break;if(!pad_outputs[i].handle){pad_outputs[i].handle=output;memset(&pad_outputs[i].colours,0,sizeof pad_outputs[i].colours);break;}}
 pthread_mutex_unlock(&pad_output_gate);pad_maybe_start();
}
static ssize_t pad_native_write(snd_rawmidi_t *output,const void *buffer,size_t size){
 pthread_mutex_lock(&pad_output_gate);ssize_t result=next_write?next_write(output,buffer,size):-ENOSYS;
 if(result>0&&(size_t)result<=size){
  RloPadColours *colours=output==pad_output?&pad_native:0;
  for(unsigned i=0;i<4;++i)if(pad_outputs[i].handle==output){if(output!=pad_output)colours=&pad_outputs[i].colours;break;}
  if(colours){if(output!=pad_output)colours->dirty=0;rlo_pad_native_colours(colours,buffer,(size_t)result);
   int has_pad=colours->dirty!=0;
   if(has_pad&&output!=pad_output){pad_restore_locked();pad_output=output;pad_lit=0;}
   if(output==pad_output){if(colours!=&pad_native)pad_native=*colours;__atomic_store_n(&pad_led_ready,rlo_pad_colours_ready(&pad_native),__ATOMIC_RELEASE);}
  }
 }
 pthread_mutex_unlock(&pad_output_gate);return result;
}
static int pad_close_handle(snd_rawmidi_t *handle){
 pthread_mutex_lock(&pad_output_gate);
 if(handle==pad_output){pad_restore_locked();pad_output=0;memset(&pad_native,0,sizeof pad_native);__atomic_store_n(&pad_led_ready,0,__ATOMIC_RELEASE);}
 for(unsigned i=0;i<4;++i)if(pad_outputs[i].handle==handle)memset(&pad_outputs[i],0,sizeof pad_outputs[i]);
 int result=next_close?next_close(handle):-ENOSYS;pthread_mutex_unlock(&pad_output_gate);return result;
}
