// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#define RLO_PAD_BRIDGE_TEST 1
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include "ButtonProtocol.h"
typedef struct FakeMidi snd_rawmidi_t;
static uint32_t clock_ms=100,mode_context=0x208330fa;
static uint32_t test_pad_context(void){return mode_context;}
static void *owner=(void*)0x1000;
static uint32_t test_pad_now(void){return clock_ms;}
static int test_pad_owner(void *effect){return effect==owner;}
static unsigned preparations;
static void test_pad_prepare(void *effect){assert(effect==owner);++preparations;}
static uint8_t wire[8192];static size_t wire_size;static int closed;
static ssize_t write_fake(snd_rawmidi_t*h,const void*b,size_t n){(void)h;assert(wire_size+n<=sizeof wire);__builtin_memcpy(wire+wire_size,b,n);wire_size+=n;return n;}
static int close_fake(snd_rawmidi_t*h){(void)h;++closed;return 0;}
static ssize_t (*next_write)(snd_rawmidi_t*,const void*,size_t)=write_fake;
static int (*next_close)(snd_rawmidi_t*)=close_fake;
#include "PadBridge.h"
static unsigned note_for(unsigned slot){return 0x36+(slot<8?56+slot:48+slot-8);}
static void release(unsigned slot){uint8_t b[]={0x89,note_for(slot),0};pad_input_bytes(b,3);}
static int attack(unsigned slot){uint8_t b[]={0x99,note_for(slot),100};pad_input_bytes(b,3);assert(b[0]==0x99&&b[1]==note_for(slot));return b[2];}
int main(void){
 RloButtonParser buttons={0};uint8_t rec[]={0xb0,0x49,127},off[]={0xb0,0x49,0};
 assert(rlo_button_bytes(&buttons,rec,2)==0&&rlo_button_bytes(&buttons,rec+2,1)==1);
 assert(!rlo_button_bytes(&buttons,rec,3));rlo_button_bytes(&buttons,off,3);
 uint8_t stream[]={0x49,127,0xf8,0x49,0};assert(rlo_button_bytes(&buttons,stream,sizeof stream)==1);
 uint8_t native[]={0x90,0x49,127,0xb0,0x50,127,0xf0,0xb0,0x49,127,0xf7};assert(!rlo_button_bytes(&buttons,native,sizeof native));
 pad_default_bank();pad_refresh_context();uint8_t rgb[48];for(unsigned i=0;i<48;++i)rgb[i]=i+1;
 assert(retrolooper_force_pad_publish(owner,11,1u|(0xffffu<<16),rgb)&RLO_PAD_PRESENT);uint32_t pad=99;
 for(unsigned i=0;i<16;++i){assert(rlo_pad_number(note_for(i))==(int)i);release(i);assert(attack(i)==0);assert(retrolooper_force_pad_take(owner,11,&pad)&&pad==i);assert(attack(i)==0&&!retrolooper_force_pad_take(owner,11,&pad));release(i);}
 assert(preparations==16);
 for(unsigned note=0x36;note<0x36+48;++note){uint8_t b[]={0x99,note,127};pad_input_bytes(b,3);assert(b[2]==127);}
 release(0);uint8_t split[]={0x99,note_for(0)};pad_input_bytes(split,2);uint8_t velocity[]={127};pad_input_bytes(velocity,1);assert(velocity[0]==0&&retrolooper_force_pad_take(owner,11,&pad)&&pad==0);
 uint8_t pressure[]={0xa9,note_for(0),88};pad_input_bytes(pressure,3);assert(!pressure[2]);release(0);
 uint8_t opaque[]={0xf0,0x47,0x99,110,127,0xf7,0xf8,0xb0,0x49,127},copy[sizeof opaque];memcpy(copy,opaque,sizeof copy);pad_input_bytes(opaque,sizeof opaque);assert(!memcmp(copy,opaque,sizeof copy));
 pad_open_output((snd_rawmidi_t*)0x505);wire_size=0;
 for(unsigned i=0;i<64;++i){uint8_t p[]={0xf0,0x47,0x7f,0x40,0x65,0,4,i,5,6,7,0xf7};assert(pad_native_write((snd_rawmidi_t*)0x505,p,5)==5);assert(pad_native_write((snd_rawmidi_t*)0x505,p+5,7)==7);}
 assert(wire_size==64*12&&pad_led_ready&&rlo_pad_colours_ready(&pad_native));wire_size=0;pad_led_tick();assert(wire_size==16*12);
 for(unsigned i=0;i<16;++i){assert(wire[i*12+7]==note_for(i)-0x36&&wire[i*12+3]==0x40);assert(!memcmp(wire+i*12+8,rgb+i*3,3));}
 wire_size=0;uint8_t upper[]={0xf0,0x47,0x7f,0x40,0x65,0,4,10,11,22,33,0xf7};pad_native_write(pad_output,upper,12);pad_led_tick();assert(wire_size==12&&!memcmp(wire,upper,12));
 /* Returning to native modes restores only the lower two rows. Pending
    looper events cannot survive a native mode round-trip. */
 release(0);assert(!attack(0));mode_context=0x208d3080;wire_size=0;pad_led_tick();assert(wire_size==192&&!pad_lit);for(unsigned i=0;i<16;++i)assert(wire[i*12+8]==5&&wire[i*12+7]>=48);
 assert(!retrolooper_force_pad_take(owner,11,&pad));release(0);assert(attack(0)==100);
 const uint32_t nativeModes[]={0x209c3080,0x20893080,0x208d30fa,0x208c30fa,0x20823080,0x208e3080,0,0xffffffff};
 for(unsigned i=0;i<sizeof nativeModes/sizeof *nativeModes;++i){mode_context=nativeModes[i];pad_led_tick();assert(!pad_owner(owner));release(0);assert(attack(0)==100);}
 mode_context=0x209c30fa;wire_size=0;pad_led_tick();assert(pad_lit&&wire_size==192);assert(!retrolooper_force_pad_take(owner,11,&pad));
 release(0);assert(!attack(0)&&retrolooper_force_pad_take(owner,11,&pad)&&pad==0);
 release(1);assert(!attack(1));clock_ms+=251;assert(!retrolooper_force_pad_take(owner,11,&pad));
 wire_size=0;pad_close_handle(pad_output);assert(closed==1&&!pad_output);wire_size=0;pad_led_tick();assert(!wire_size);
 puts("PASS: Force CC REC, split/running MIDI, all 16 lower-row pad identities, untouched upper 48 pads, pressure and SysEx preservation, learned RGB headers, native colour restoration, editor-only mode handoff and stale-event rejection");return 0;
}
