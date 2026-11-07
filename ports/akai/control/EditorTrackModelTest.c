// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#define RLO_TRACK_BRIDGE_TEST 1
#define RLO_TRACK_STATE_PATH "/tmp/retrolooper-editor-model-test.json"
#include <assert.h>
static void test_bool_set(void*,unsigned char);
static void test_monitor_set(void*,unsigned);
#include "TrackBridge.h"
typedef struct FakeMidi snd_rawmidi_t;
static ssize_t (*next_write)(snd_rawmidi_t*,const void*,size_t);
static int (*next_close)(snd_rawmidi_t*);
#include "PadBridge.h"
static uint32_t project[800],pool[128],tracks[3],track[3][512],program[2][4096];
static uint32_t chain[2][128],effect[3][64],wrapper[3][64],fx[3][64],slots[2][4];
static uint32_t app[64],window[160],engine[256],touch[128],container[100],editor[128],props[64],session[32],children[4],mode[64];
static unsigned selections,monitor_writes;
static void put(void *p,unsigned off,uint32_t v){*(uint32_t*)((char*)p+off)=v;}
static void select_track(void *property,uint32_t value){assert(property==(char*)project+0x78);assert(value==(uintptr_t)track[0]||value==(uintptr_t)track[1]);++selections;put(project,0xa0,value);}
static void test_monitor_set(void *property,unsigned v){uintptr_t pg=(uintptr_t)property-0x26c8;assert(pg==(uintptr_t)program[0]||pg==(uintptr_t)program[1]);put((void*)pg,0x26f0,v);++monitor_writes;}
static void test_bool_set(void *property,unsigned char v){(void)property;(void)v;assert(!"Unexpected arm mutation");}
static void show(unsigned pg,unsigned slot){put(mode,0x6c,0x30fa);put(session,0x34,(uintptr_t)program[pg]+0x3c);put(session,0x38,slot);}
int main(void){
 native_mem=open("/proc/self/mem",O_RDONLY|O_CLOEXEC);assert(native_mem>=0);
 native_supported=native_enabled=1;native_project=(uintptr_t)project;native_engine_mode=(uintptr_t)mode;
 test_editor_application=(uintptr_t)app;test_editor_select=select_track;
 put(project,0,0x6d501c4);put(project,0x94,1);put(project,0xa0,(uintptr_t)track[0]);put(project,0xb1c,(uintptr_t)pool);
 put(pool,0,0x6cb7528);put(pool,0x60,(uintptr_t)tracks);put(pool,0x64,(uintptr_t)(tracks+2));
 for(unsigned i=0;i<2;++i){
  tracks[i]=(uintptr_t)track[i];put(track[i],0,0x6d51624);put(track[i],0x64c,6);put(track[i],0x648,(uintptr_t)program[i]);
  put(program[i],0,0x6d4c640);put(program[i],0x3c,0x6d4c7fc);put(program[i],0x5e0,(uintptr_t)chain[i]);put(program[i],0x26e4,1);
  put(chain[i],0,0x6cb6aa8);put(chain[i],0x60,(uintptr_t)slots[i]);put(chain[i],0x64,(uintptr_t)(slots[i]+1));slots[i][0]=(uintptr_t)effect[i];
 }
 for(unsigned i=0;i<3;++i){put(effect[i],0,0x6d4b5f8);put(effect[i],0x68,(uintptr_t)wrapper[i]);put(wrapper[i],0,0x6c90c90);put(wrapper[i],0xcc,(uintptr_t)fx[i]);fx[i][0]=0x56737450;fx[i][11]=(uintptr_t)wrapper[i];fx[i][18]=0x526c4f31;}
 put(app,0,0x6cdfaf4);put(app,0xc4,(uintptr_t)window);put(window,0,0x6cdfe94);put(window,0x274,(uintptr_t)touch+4);
 put(window,0x26c,(uintptr_t)engine);put(engine,0,0x6ce5650);put(engine,0x264,(uintptr_t)project);put(engine,0x118,(uintptr_t)mode);
 put(touch,0,0x6dafdd0);put(touch,4,0x6dafe14);put(touch,0xa4,(uintptr_t)container);
 put(container,0,0x6dd3d3c);put(container,0x10,0x6dd3de4);put(container,0x38,(uintptr_t)children);put(container,0x40,1);
 children[0]=(uintptr_t)editor+8;put(editor,0,0x6ddac38);put(editor,8,0x6ddacac);((unsigned char*)editor)[8+0x68]=2;
 put(editor,8+0xc,(uintptr_t)container+0x10);
 put(editor,0x1ac,(uintptr_t)props);put(props,0x5c,(uintptr_t)session);put(session,0,0x6ca95a0);put(mode,0,0x6ca3344);put(mode,0x40,0x2083);
 show(1,0);RloEditorOwner owner;assert(editor_owner(&owner)&&owner.track==(uintptr_t)track[1]&&owner.effect==(uintptr_t)fx[1]);
 ui_refresh();assert(selections==1&&native_word((uintptr_t)project+0xa0)==(uintptr_t)track[1]);
 assert(visible_editor_fx==(uintptr_t)fx[1]&&snapshots[snapshot_active].editor_fx==(uintptr_t)fx[1]);
 assert(native_word((uintptr_t)program[1]+0x26f0)==1&&native_word((uintptr_t)program[0]+0x26f0)==0);
 ui_refresh();assert(selections==1); /* Repeated parameter/UI polling never selects again. */
 put(project,0xa0,(uintptr_t)track[0]);ui_refresh();assert(selections==1&&native_word((uintptr_t)project+0xa0)==(uintptr_t)track[0]);
 pad_default_bank();pad_refresh_context();assert(!pad_owner(fx[0])&&!pad_owner(fx[1])); /* A mismatched visible editor cannot trigger the other track. */
 put(mode,0x6c,0x3080);ui_refresh();assert(!visible_editor_fx);show(1,0);ui_refresh();assert(selections==2);
 pad_refresh_context();assert(pad_owner(fx[1])&&!pad_owner(fx[0]));
 /* A second RetroLooper on the same track owns the pad grid when shown. */
 slots[1][1]=(uintptr_t)effect[2];put(chain[1],0x64,(uintptr_t)(slots[1]+2));show(1,1);ui_refresh();pad_refresh_context();
 assert(selections==2&&visible_editor_fx==(uintptr_t)fx[2]&&pad_owner(fx[2])&&!pad_owner(fx[1]));
 show(0,0);ui_refresh();assert(selections==3&&pad_owner(fx[0]));
 /* Normal modes, hidden editors, other plugins and invalid native owners
    must never select a track or grant that editor ownership. */
 put(mode,0x6c,0x3080);ui_refresh();show(1,0);((unsigned char*)editor)[8+0x68]=0;ui_refresh();assert(!visible_editor_fx&&selections==3);
 ((unsigned char*)editor)[8+0x68]=2;fx[1][18]=0x12345678;ui_refresh();assert(!visible_editor_fx&&selections==3&&visible_editor_open&&!pad_owner(fx[0]));fx[1][18]=0x526c4f31;
 put(session,0x38,16);ui_refresh();assert(!visible_editor_fx&&selections==3);show(1,0);
 tracks[2]=(uintptr_t)track[2];put(track[2],0,0x6d51624);put(track[2],0x64c,6);put(track[2],0x648,(uintptr_t)program[1]);put(pool,0x64,(uintptr_t)(tracks+3));
 assert(!editor_owner(&owner));put(pool,0x64,(uintptr_t)(tracks+1));assert(!editor_owner(&owner));put(pool,0x64,(uintptr_t)(tracks+2));
 put(track[1],0x64c,3);assert(!editor_owner(&owner));put(track[1],0x64c,6);
 children[1]=children[0];put(container,0x40,2);assert(!editor_owner(&owner));put(container,0x40,1);
 put(project,0x94,0);ui_refresh();assert(selections==3);put(project,0x94,1);put(mode,0x6c,0x3080);ui_refresh();show(1,0);ui_refresh();assert(selections==4);
 /* Track Mute/Pad Mute remain native owners. Returning to the visible
    looper restores its pad grid and revalidates the actual track. */
 put(mode,0x40,0x208d);put(mode,0x6c,0x3080);ui_refresh();pad_refresh_context();assert(!pad_owner(fx[1])&&!visible_editor_fx);
 put(mode,0x40,0x208c);pad_refresh_context();assert(!pad_owner(fx[1]));put(mode,0x40,0x2083);show(1,0);ui_refresh();pad_refresh_context();assert(pad_owner(fx[1]));
 test_editor_application=0;ui_refresh();assert(!visible_editor_fx&&selections==4);
 close(native_mem);unlink(RLO_TRACK_STATE_PATH);
 dprintf(2,"PASS: visible native insert resolves exact owner; notifying selection once per opened/changed instance; selected-only monitor routing; second-insert pad ownership; no selection fights; hidden/other-plugin/deleted/ambiguous/corrupt ownership rejected; native-mode pad handoff preserved\n");
 return 0;
}
