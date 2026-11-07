// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#define RLO_PAD_BRIDGE_TEST 1
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
typedef struct FakeMidi snd_rawmidi_t;
static uint32_t clock_ms=100;
static uint32_t mode_context=0x20803080;
static uint32_t test_pad_context(void){return mode_context;}
static void *owner=(void*)0x1000;
static uint32_t test_pad_now(void){return clock_ms;}
static int test_pad_owner(void *effect){return effect==owner;}
static unsigned preparations;
static void test_pad_prepare(void *effect){assert(effect==owner);++preparations;}
static uint8_t wire[8192];static size_t wire_size;static int closed;
static ssize_t write_fake(snd_rawmidi_t*h,const void*b,size_t n){(void)h;assert(wire_size+n<=sizeof wire);__builtin_memcpy(wire+wire_size,b,n);wire_size+=n;return (ssize_t)n;}
static int close_fake(snd_rawmidi_t*h){(void)h;++closed;return 0;}
static ssize_t (*next_write)(snd_rawmidi_t*,const void*,size_t)=write_fake;
static int (*next_close)(snd_rawmidi_t*)=close_fake;
#include "PadBridge.h"
static void button(unsigned note,unsigned velocity){uint8_t b[]={0x90,(uint8_t)note,(uint8_t)velocity};pad_input_bytes(b,sizeof b);assert(b[0]==0x90&&b[1]==note&&b[2]==velocity);}
static void release(unsigned note){uint8_t b[]={0x89,(uint8_t)note,0};pad_input_bytes(b,sizeof b);}
int main(void){
 pad_refresh_context();
 uint8_t rgb[48];for(unsigned i=0;i<48;++i)rgb[i]=(uint8_t)(i+1);
 assert(retrolooper_mpc_pad_publish(owner,11,1u|(1u<<16),rgb)==RLO_PAD_PRESENT);
 uint8_t first[]={0x99,37,127};pad_input_bytes(first,3);assert(first[2]==127);
 button(35,127);assert(pad_bank==0);
 first[2]=127;pad_input_bytes(first,3);assert(first[2]==0);uint32_t pad=99;
 assert(retrolooper_mpc_pad_take(owner,11,&pad)&&pad==0);
 assert(preparations==1);
 first[2]=127;pad_input_bytes(first,3);assert(first[2]==0&&!retrolooper_mpc_pad_take(owner,11,&pad));
 uint8_t pressure[]={0xa9,37,88};pad_input_bytes(pressure,3);assert(pressure[2]==0);release(37);
 uint8_t split[]={0x99,36};pad_input_bytes(split,2);assert(split[0]==0x99&&split[1]==36);
 uint8_t velocity[]={127};pad_input_bytes(velocity,1);assert(velocity[0]==0&&retrolooper_mpc_pad_take(owner,11,&pad)&&pad==1);
 button(36,127);release(37);first[2]=127;pad_input_bytes(first,3);assert(first[2]==127&&!retrolooper_mpc_pad_take(owner,11,&pad));
 button(49,127);button(35,127);assert(pad_bank==4);button(49,0);first[2]=127;pad_input_bytes(first,3);assert(first[2]==127);
 button(35,127);owner=(void*)0x2000;release(37);first[2]=127;pad_input_bytes(first,3);assert(first[2]==127);
 owner=(void*)0x1000;retrolooper_mpc_pad_publish(owner,12,1,rgb);release(37);first[2]=127;pad_input_bytes(first,3);
 assert(!retrolooper_mpc_pad_take(owner,11,&pad));
 release(37);first[2]=127;pad_input_bytes(first,3);clock_ms+=251;assert(!retrolooper_mpc_pad_take(owner,12,&pad));
 uint8_t untouched[]={0xf0,0x47,0x99,37,127,0xf7,0xf8,0x90,73,127};uint8_t copy[sizeof untouched];memcpy(copy,untouched,sizeof copy);pad_input_bytes(untouched,sizeof untouched);assert(!memcmp(copy,untouched,sizeof copy));
 static const uint8_t notes[16]={37,36,42,82,40,38,46,44,48,47,45,43,49,55,51,53};
 for(unsigned i=0;i<16;++i)assert(rlo_pad_number(notes[i])==(int)i);
 pad_output=(snd_rawmidi_t*)0x303;
 for(unsigned i=0;i<16;++i){uint8_t p[]={0xf0,0x47,0x7f,0x3b,0x65,0,4,(uint8_t)i,5,6,7,0xf7};assert(pad_native_write(pad_output,p,5)==5);assert(pad_native_write(pad_output,p+5,7)==7);}
 assert(pad_led_ready&&rlo_pad_colours_ready(&pad_native));wire_size=0;pad_refresh_context();retrolooper_mpc_pad_publish(owner,12,1,rgb);pad_led_tick();
 assert(wire_size==192);for(unsigned i=0;i<16;++i){assert(wire[i*12+3]==0x3b&&wire[i*12+7]==i);assert(!memcmp(wire+i*12+8,rgb+i*3,3));assert(pad_native.bytes[i][8]==5);}
 wire_size=0;pad_led_tick();assert(wire_size==0);
 uint8_t update[]={0xf0,0x47,0x7f,0x3b,0x65,0,4,3,8,9,10,0xf7};pad_native_write(pad_output,update,12);assert(wire_size==12&&!memcmp(wire,update,12));pad_led_tick();assert(wire_size==24&&!memcmp(wire+20,rgb+9,3));
 wire_size=0;retrolooper_mpc_pad_publish(owner,12,0,rgb);pad_led_tick();assert(wire_size==192&&wire[3*12+8]==8&&wire[3*12+9]==9);
 wire_size=0;assert(pad_close_handle((snd_rawmidi_t*)0x303)==0&&closed==1);pad_led_tick();assert(wire_size==0);
 /* Real One+ uses a separate RGB output stream; button LED writes must not steal it. */
 pad_default_bank();assert(pad_bank==0&&pad_parser.known);
 pad_open_output((snd_rawmidi_t*)0x404);pad_open_output((snd_rawmidi_t*)0x505);
 const uint8_t tap[]={0xb0,0x35,3};assert(pad_native_write((snd_rawmidi_t*)0x404,tap,3)==3);assert(!pad_output);
 wire_size=0;
 for(unsigned i=0;i<16;i++){uint8_t p[]={0xf0,0x47,0x7f,0x56,0x65,0,4,(uint8_t)i,0,0,0,0xf7};assert(pad_native_write((snd_rawmidi_t*)0x505,p,12)==12);}
 assert(pad_output==(snd_rawmidi_t*)0x505&&pad_led_ready);wire_size=0;retrolooper_mpc_pad_publish(owner,12,1,rgb);pad_led_tick();assert(wire_size==192&&wire[3]==0x56);
 wire_size=0;pad_native_write((snd_rawmidi_t*)0x404,tap,3);pad_led_tick();assert(wire_size==3&&pad_output==(snd_rawmidi_t*)0x505);
 /* Native modes own both input and LEDs. No queued looper press may leak
    across a mode change, including a quick trip away and back. */
 release(37);first[2]=127;pad_input_bytes(first,3);assert(first[2]==0);
 mode_context=0x208d3080;wire_size=0;pad_led_tick();assert(wire_size==192&&!pad_lit);
 assert(!(retrolooper_mpc_pad_publish(owner,12,1,rgb)&RLO_PAD_ACTIVE));
 assert(!retrolooper_mpc_pad_take(owner,12,&pad));
 release(37);first[2]=127;pad_input_bytes(first,3);assert(first[2]==127);
 wire_size=0;pad_led_tick();assert(wire_size==0);
 uint8_t nativeUpdate[]={0xf0,0x47,0x7f,0x56,0x65,0,4,0,11,22,33,0xf7};
 assert(pad_native_write(pad_output,nativeUpdate,12)==12);wire_size=0;pad_led_tick();assert(wire_size==0);
 mode_context=0x208c3080;pad_led_tick();release(37);first[2]=127;pad_input_bytes(first,3);assert(first[2]==127);
 mode_context=0x208330fa;wire_size=0;pad_led_tick();assert(wire_size==192&&pad_lit);assert(!retrolooper_mpc_pad_take(owner,12,&pad));
 retrolooper_mpc_pad_publish(owner,12,1,rgb);wire_size=0;pad_led_tick();assert(wire_size==0&&pad_lit);
 release(37);first[2]=127;pad_input_bytes(first,3);assert(first[2]==0&&retrolooper_mpc_pad_take(owner,12,&pad)&&pad==0);
 const uint32_t nativeModes[]={0x20823080,0x208e3080,0x20913080,0x20953080,0x209c3080,0x20803095,0x208030e1,0x208030f0,0x208d30fa,0,0xffffffff};
 for(unsigned i=0;i<sizeof nativeModes/sizeof *nativeModes;++i){mode_context=nativeModes[i];pad_led_tick();assert(!pad_owner(owner));release(37);first[2]=127;pad_input_bytes(first,3);assert(first[2]==127);}
 mode_context=0x20803080;pad_led_tick();retrolooper_mpc_pad_publish(owner,12,1,rgb);
 pad_close_handle((snd_rawmidi_t*)0x404);assert(pad_output==(snd_rawmidi_t*)0x505);pad_close_handle((snd_rawmidi_t*)0x505);assert(!pad_output);
 dprintf(2,"PASS: owned Bank A routing, native pad identities, split/running status, pressure/SysEx isolation, native-mode input and LED handoff, rapid-return stale-event rejection, unknown-mode fallback, learned native colour updates/restoration and output-close lifetime\n");
 return 0;
}
