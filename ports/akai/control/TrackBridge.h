// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
/* MPC One+ 3.10.0.49 native selection/arm bridge.
   The queue ABI and enqueue checks are adapted from Twisted Cable's MIT
   plugin-manager-usb-for-open-mpc/src/startup/native_ui.cpp.
   No executable patches, saved-project selection, UI focus, or audio I/O.
   All native model mutation runs through MPC's UI queue. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "TrackProtocol.h"
#if defined(__arm__) && UINTPTR_MAX == 0xffffffffu
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <link.h>
#include <sys/syscall.h>
#include <sys/stat.h>

#ifndef RLO_TRACK_STATE_PATH
#define RLO_TRACK_STATE_PATH "/tmp/retrolooper-track-state.json"
#endif
/* Pinned native AudioProgram enum. Human-selected IN was read as 1 on this
   firmware; 2 is AUTO and cuts live input when native REC ARM is Off. */
enum { MAX_TRACK_FX=16,MAX_NATIVE_TRACKS=512,MONITOR_OFF=0,MONITOR_IN=1,MONITOR_AUTO=2,MONITOR_MERGE=3 };
typedef struct {void(*invoke)(void*);void(*manage)(int,void*,void*);unsigned char data[144];} RloUiCallback;
_Static_assert(sizeof(RloUiCallback)==152,"Pinned ARM32 native queue ABI");
static uintptr_t native_base,native_project,native_queues,native_engine_mode;
static int native_mem=-1;
static uint32_t native_supported,native_enabled,ui_pending,request_gen,done_gen,want_fx,monitor_tracks,last_monitor_track,last_monitor_program;
typedef struct {uint32_t project,track,arm,count,stamp,monitor,fx[MAX_TRACK_FX],editor_fx,editor_open;} RloTrackSnapshot;
static RloTrackSnapshot snapshots[2];
static uint32_t snapshot_active,snapshot_readers[2];
static int snapshot_read(RloTrackSnapshot *out){
 for(int tries=0;tries<2;tries++){
  unsigned slot=__atomic_load_n(&snapshot_active,__ATOMIC_ACQUIRE);
  __atomic_fetch_add(&snapshot_readers[slot],1,__ATOMIC_ACQ_REL);
  if(slot!=__atomic_load_n(&snapshot_active,__ATOMIC_ACQUIRE)){__atomic_fetch_sub(&snapshot_readers[slot],1,__ATOMIC_RELEASE);continue;}
  *out=snapshots[slot];
  __atomic_fetch_sub(&snapshot_readers[slot],1,__ATOMIC_RELEASE);return 1;
 }
 return 0;
}
static pthread_once_t track_once=PTHREAD_ONCE_INIT;
static int native_read(void *out,uintptr_t address,size_t count){return address&&pread(native_mem,out,count,(off_t)address)==(ssize_t)count;}
static uint32_t native_word(uintptr_t address){uint32_t word=0;native_read(&word,address,4);return word;}
static uint32_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint32_t)((uint64_t)t.tv_sec*1000+t.tv_nsec/1000000);}
static int word_is(uintptr_t address,uint32_t word){uint32_t got=0;return native_read(&got,address,4)&&got==word;}
static int main_image(struct dl_phdr_info *i,size_t n,void *data){(void)n;(void)data;if(!i->dlpi_name||!i->dlpi_name[0]){native_base=i->dlpi_addr;return 1;}return 0;}
static int native_fingerprint(void){
 static const unsigned char build[20]={0xbc,0x14,0x10,0x62,0xea,0x5e,0xa5,0xbe,0x61,0x9d,0x98,0x36,0x6f,0x42,0x3c,0x81,0x02,0x57,0x43,0x65};
 unsigned char header[1024];int fd=open("/proc/self/exe",O_RDONLY|O_CLOEXEC);if(fd<0)return 0;
 ssize_t size=read(fd,header,sizeof header);close(fd);int found=0;
 for(int i=0;i+20<=size;i++)if(!memcmp(header+i,build,20))found=1;
 if(!found)return 0;
 return word_is(native_base+0x2d2b7b8,0xe16d41f8)&&word_is(native_base+0x2cb83f8,0xe5904068)
     &&word_is(native_base+0x2caebec,0xe5d40483)&&word_is(native_base+0xe5e5d8,0xe5d0300f)
     &&word_is(native_base+0x1923704,0xe5953028)&&word_is(native_base+0x19236c8,0xe280a010)
     &&word_is(native_base+0x291641c,0xe5845b1c)&&word_is(native_base+0x2932390,0xe5904060)
     &&word_is(native_base+0x6cb951c,native_base+0x2d2b7b8)
     &&word_is(native_base+0x165c5cc,0xe5900040)&&word_is(native_base+0x6ca334c,native_base+0x165c5cc);
}
static int next_line(char **p,char *line,size_t cap){
 if(!**p)return 0;char *end=strchr(*p,10);size_t n=end?(size_t)(end-*p):strlen(*p);if(n>=cap)n=cap-1;
 memcpy(line,*p,n);line[n]=0;*p=end?end+1:*p+strlen(*p);return 1;
}
/* Startup discovery only: bounded to the first 32 MiB of the ordinary heap.
   It does not scan loop audio or add work to an audio callback. */
static void discover_model(void){
 char map[32768],line[512],*cursor=map;int fd=open("/proc/self/maps",O_RDONLY|O_CLOEXEC);if(fd<0)return;
 ssize_t n=read(fd,map,sizeof map-1);close(fd);if(n<=0)return;map[n]=0;
 while(next_line(&cursor,line,sizeof line)){
  unsigned a,b;char perm[8];if(!strstr(line,"[heap]")||sscanf(line,"%x-%x %7s",&a,&b,perm)!=3||perm[0]!='r')continue;
  uint32_t block[16384];uint64_t end=b;if(end>(uint64_t)a+32*1024*1024)end=(uint64_t)a+32*1024*1024;
  for(uint64_t at=a;at<end;at+=sizeof block){
   size_t bytes=end-at<sizeof block?end-at:sizeof block;ssize_t got=pread(native_mem,block,bytes,(off_t)at);if(got<=0)continue;
   for(size_t i=0;i<(size_t)got/4;i++){
    uintptr_t p=at+i*4;
    if(!native_project&&block[i]==native_base+0x6d501c4)native_project=p;
    if(!native_queues&&block[i]==native_base+0x6cb958c&&word_is(p+0x18,native_base+0x6cb9514))native_queues=p;
    if(!native_engine_mode&&block[i]==native_base+0x6ca3344)native_engine_mode=p;
   }
   if(native_project&&native_queues&&native_engine_mode)return;
  }
 }
}
static int track_effects(uint32_t track,uint32_t *program_out,uint32_t *arm,uint32_t *fx,unsigned *count){
 *program_out=*arm=0;*count=0;
 if(!track)return 1;
 if(!word_is(track,native_base+0x6d51624))return 0;
 unsigned char armed=0;if(!native_read(&armed,track+0x483,1)||armed>1)return 0;*arm=armed;
 if(native_word(track+0x64c)!=6)return 1; /* Only native Audio tracks. */
 uint32_t program=native_word(track+0x648);
 if(!word_is(program,native_base+0x6d4c640))return 0;
 *program_out=program;
 uint32_t inserts=native_word(program+0x5e0);
 if(!word_is(inserts,native_base+0x6cb6aa8))return 0;
 uint32_t begin=native_word(inserts+0x60),end=native_word(inserts+0x64);
 if(end<begin||(end-begin)%4||end-begin>MAX_TRACK_FX*4)return 0;
 for(uint32_t p=begin;p<end;p+=4){
  uint32_t effect=native_word(p);if(!effect)continue;
  if(!word_is(effect,native_base+0x6d4b5f8))return 0;
  uint32_t wrapper=native_word(effect+0x68);
  if(!word_is(wrapper,native_base+0x6c90c90))continue; /* Stock AIR and non-VST inserts. */
  uint32_t candidate=native_word(wrapper+0xcc),words[20];
  if(!native_read(words,candidate,sizeof words)||words[0]!=0x56737450||words[18]!=0x526c4f31)continue;
  if(words[11]!=wrapper)return 0; /* The host-owned resvd2 must agree. */
  fx[(*count)++]=candidate;
 }
 return 1;
}
static int selected_effects(uint32_t *track,uint32_t *arm,uint32_t *fx,unsigned *count){
 *track=*arm=0;*count=0;
 if(!word_is(native_project,native_base+0x6d501c4))return 0;
 *track=native_word(native_project+0xa0);uint32_t program=0;
 return track_effects(*track,&program,arm,fx,count)&&native_word(native_project+0xa0)==*track;
}
static int monitor_set(uint32_t program,uint32_t value){
 if(value>MONITOR_MERGE)return 0;
 if(!word_is(program,native_base+0x6d4c640)||native_word(program+0x26f0)>MONITOR_MERGE
    ||native_word(program+0x26e4)!=1)return 0; /* Native recursive mutex. */
 if(native_word(program+0x26f0)==value)return 1;
#ifdef RLO_TRACK_BRIDGE_TEST
 test_monitor_set((void*)(uintptr_t)(program+0x26c8),value);
#else
 /* AudioProgramConnector's own monitor callback (0x21a7008) uses this
    locking, notifying enum setter. The UI-only enum setter has a different
    layout and must never be used on the AudioProgram's backend property. */
 ((void(*)(void*,uint32_t))(native_base+0x192369c))((void*)(uintptr_t)(program+0x26c8),value);
#endif
 return native_word(program+0x26f0)==value;
}
static int route_native_monitors(uint32_t selected,int force_input){
 monitor_tracks=0;
 if(!word_is(native_project,native_base+0x6d501c4))return 0;
 uint32_t pool=native_word(native_project+0xb1c);
 if(!word_is(pool,native_base+0x6cb7528))return 0;
 uint32_t begin=native_word(pool+0x60),end=native_word(pool+0x64);
 if(end<begin||(end-begin)%4||end-begin>MAX_NATIVE_TRACKS*4)return 0;
 uint32_t tracks[MAX_NATIVE_TRACKS];unsigned total=(end-begin)/4;
 if(total&&!native_read(tracks,begin,total*4))return 0;
 struct {uint32_t track,program,desired;} routes[MAX_NATIVE_TRACKS];unsigned routes_count=0;
 int selected_found=!selected;uint32_t selected_program=0;
 /* Preflight every owner before changing a property. The live TrackPool
    supplies ownership; no cached Track is mutated after deletion/reload. */
 for(unsigned i=0;i<total;++i){
  if(tracks[i]==selected)selected_found=1;
  uint32_t program=0,arm=0,fx[MAX_TRACK_FX];unsigned count=0;
  if(!track_effects(tracks[i],&program,&arm,fx,&count))return 0;
  if(!count)continue;
  /* MPC also saves MERGE (3). It is a valid starting/manual mode, not a
     corrupt property. Selection/capture preparation still chooses IN. */
  if(native_word(program+0x26f0)>MONITOR_MERGE||native_word(program+0x26e4)!=1)return 0;
  for(unsigned j=0;j<routes_count;++j)if(routes[j].program==program)return 0;
  routes[routes_count].track=tracks[i];routes[routes_count].program=program;
  uint32_t desired=MONITOR_OFF;
  if(tracks[i]==selected){
   selected_program=program;
   desired=force_input||selected!=last_monitor_track||program!=last_monitor_program?MONITOR_IN:native_word(program+0x26f0);
  }
  routes[routes_count++].desired=desired;
 }
 if(!selected_found||native_word(native_project+0xa0)!=selected)return 0;
 /* Close old live-input paths before opening the selected one. Existing
    timeline clips and insert-loop outputs continue through MPC normally. */
 for(unsigned pass=0;pass<2;++pass)for(unsigned i=0;i<routes_count;++i){
  if((routes[i].desired==MONITOR_IN)!=(pass==1))continue;
  if(native_word(native_project+0xa0)!=selected||!monitor_set(routes[i].program,routes[i].desired))return 0;
 }
 if(native_word(native_project+0xa0)!=selected)return 0;
 for(unsigned i=0;i<routes_count;++i)if(native_word(routes[i].program+0x26f0)!=routes[i].desired)return 0;
 monitor_tracks=routes_count;last_monitor_track=selected;last_monitor_program=selected_program;return 1;
}
static void native_disarm(uint32_t track){
 /* Mirror ScriptableTrackPool::SetTrackRecordArmed's filtered-property path,
    including its override, then use the host's notifying property setter. */
 unsigned char value=0;
 if(native_word(track+0x48c)){
  uintptr_t fn=native_word(track+0x490);if(!fn)return;
  value=((unsigned char(*)(void*,const unsigned char*))fn)((void*)(uintptr_t)(track+0x484),&value);
 }
#ifdef RLO_TRACK_BRIDGE_TEST
 test_bool_set((void*)(uintptr_t)(track+0x474),value);
#else
 ((void(*)(void*,unsigned char))(native_base+0xe5e5d4))((void*)(uintptr_t)(track+0x474),value);
#endif
}
#include "EditorTrack.h"
static void ui_refresh(void){
 if(!__atomic_load_n(&native_enabled,__ATOMIC_ACQUIRE)){__atomic_store_n(&ui_pending,0,__ATOMIC_RELEASE);return;}
 if(syscall(SYS_gettid)!=getpid()){__atomic_store_n(&ui_pending,0,__ATOMIC_RELEASE);return;}
 follow_editor_track();
 uint32_t fx[MAX_TRACK_FX]={0},track=0,arm=0;unsigned count=0;
 uint32_t request=__atomic_load_n(&request_gen,__ATOMIC_ACQUIRE),wanted=__atomic_load_n(&want_fx,__ATOMIC_ACQUIRE);
 int valid=selected_effects(&track,&arm,fx,&count);
 int requested=request!=__atomic_load_n(&done_gen,__ATOMIC_ACQUIRE),owns=!wanted;
 for(unsigned i=0;i<count;i++)if(fx[i]==wanted)owns=1;
 if(valid&&requested&&count&&owns&&arm){
  native_disarm(track);
  valid=selected_effects(&track,&arm,fx,&count);
 }
 if(valid)valid=route_native_monitors(track,requested&&count&&owns);
 if(valid)valid=selected_effects(&track,&arm,fx,&count);
 int monitor=valid&&count?(int)native_word(native_word(track+0x648)+0x26f0):-1;
 unsigned slot=__atomic_load_n(&snapshot_active,__ATOMIC_RELAXED)^1;int published=0;
 if(!__atomic_load_n(&snapshot_readers[slot],__ATOMIC_ACQUIRE)){
  RloTrackSnapshot *next=&snapshots[slot];
  next->project=valid?(uint32_t)native_project:0;next->track=valid?track:0;next->arm=arm;next->count=valid?count:0;next->stamp=now_ms();next->monitor=(uint32_t)monitor;
  next->editor_fx=visible_editor_fx;next->editor_open=visible_editor_open;memcpy(next->fx,fx,sizeof fx);__atomic_store_n(&snapshot_active,slot,__ATOMIC_RELEASE);published=1;
 }
 if(requested&&published)__atomic_store_n(&done_gen,request,__ATOMIC_RELEASE);
 static uint32_t previous_track,previous_arm=2,previous_request,previous_count,previous_routes,previous_editor,previous_editor_open,previous_selections;
 static int previous_valid=-1,previous_monitor=-2;
 if(published&&(track!=previous_track||arm!=previous_arm||request!=previous_request||count!=previous_count
     ||valid!=previous_valid||monitor!=previous_monitor||monitor_tracks!=previous_routes||visible_editor_fx!=previous_editor||visible_editor_open!=previous_editor_open||editor_selection_count!=previous_selections)){
  int fd=open(RLO_TRACK_STATE_PATH,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
  if(fd>=0){dprintf(fd,"{\"ready\":%d,\"track\":%u,\"arm\":%u,\"count\":%u,\"request\":%u,\"ui_thread\":true,\"monitor\":%d,\"monitor_tracks\":%u,\"editor_open\":%u,\"editor_fx\":%u,\"editor_selections\":%u,\"fx\":[",valid,track,arm,count,request,monitor,monitor_tracks,visible_editor_open,visible_editor_fx,editor_selection_count);
   for(unsigned i=0;i<count;i++)dprintf(fd,"%s%u",i?",":"",fx[i]);dprintf(fd,"]}\n");close(fd);}
  previous_track=track;previous_arm=arm;previous_request=request;previous_count=count;
  previous_valid=valid;previous_monitor=monitor;previous_routes=monitor_tracks;
  previous_editor=visible_editor_fx;previous_editor_open=visible_editor_open;previous_selections=editor_selection_count;
 }
 __atomic_store_n(&ui_pending,0,__ATOMIC_RELEASE);
}
static void ui_invoke(void *data){memset(data,0,sizeof(uintptr_t));ui_refresh();}
static void ui_manage(int operation,void *source,void *destination){
 if(operation==0)memcpy(destination,source,144);
 uintptr_t token=0;memcpy(&token,source,sizeof token);
 if(operation==1&&token)__atomic_store_n(&ui_pending,0,__ATOMIC_RELEASE);
 if(operation==0||operation==1)memset(source,0,144);
}
static int ui_post(void){
 if(!__atomic_load_n(&native_enabled,__ATOMIC_ACQUIRE)||!word_is(native_project,native_base+0x6d501c4)||!word_is(native_queues,native_base+0x6cb958c)||!word_is(native_queues+0x18,native_base+0x6cb9514))return 0;
 if(__atomic_exchange_n(&ui_pending,1,__ATOMIC_ACQ_REL))return 0;
 unsigned char *queue=(unsigned char*)(native_queues+0x18);
 void *mutex=queue+0x2c;
 if(!((int(*)(void*))(native_base+0x96fb64))(mutex)){__atomic_store_n(&ui_pending,0,__ATOMIC_RELEASE);return 0;}
 uint32_t *raw=(uint32_t*)(queue+4);
 ((void(*)(void*))(native_base+0x2d32370))(raw);
 uint32_t capacity=raw[0],write=raw[4],boundary=raw[2];int accepted=0;
 RloUiCallback cb={ui_invoke,ui_manage,{0}};uintptr_t token=1;memcpy(cb.data,&token,sizeof token);
 if(!((unsigned char*)raw)[0x19]&&capacity>1&&capacity<=1048576&&write<capacity&&boundary<capacity&&raw[7]){
  uint32_t next=write+1==capacity?0:write+1;uintptr_t state=raw[7]+(uintptr_t)write*160+152;
  if(next!=boundary&&state>=raw[7]&&(__atomic_load_n((uint32_t*)state,__ATOMIC_ACQUIRE)&3)==0)
   accepted=((int(*)(void*,RloUiCallback*,uintptr_t))(native_base+0x2d3299c))(raw,&cb,0);
 }
 ((void(*)(void*))(native_base+0x96fb84))(mutex);
 memcpy(&token,cb.data,sizeof token);
 if(!accepted&&token)__atomic_store_n(&ui_pending,0,__ATOMIC_RELEASE);
 return accepted;
}
static void *track_worker(void *ignored){
 (void)ignored;
 uint32_t last_discovery=now_ms()-1000;
 for(;;){
  if((!native_project||!native_queues||!native_engine_mode)&&now_ms()-last_discovery>=1000){discover_model();last_discovery=now_ms();}
  if(native_project&&native_queues&&__atomic_load_n(&native_enabled,__ATOMIC_ACQUIRE))ui_post();
  struct timespec pause={0,native_project&&native_queues?20000000:250000000};nanosleep(&pause,0);
 }
 return 0;
}
static void track_init(void){
 char exe[256];ssize_t n=readlink("/proc/self/exe",exe,sizeof exe-1);if(n<1)return;exe[n]=0;
 char *name=strrchr(exe,'/');if(!name||strcmp(name+1,"MPC"))return;
 dl_iterate_phdr(main_image,0);native_mem=open("/proc/self/mem",O_RDONLY|O_CLOEXEC);
 if(native_mem<0||!native_fingerprint())return;
 __atomic_store_n(&native_supported,1,__ATOMIC_RELEASE);
 pthread_t thread;if(pthread_create(&thread,0,track_worker,0)==0)pthread_detach(thread);
}
static void rlo_track_start(void){__atomic_store_n(&native_enabled,1,__ATOMIC_RELEASE);pthread_once(&track_once,track_init);}
static void rlo_track_pause(void){__atomic_store_n(&native_enabled,0,__ATOMIC_RELEASE);}
__attribute__((visibility("default"))) uint32_t retrolooper_mpc_track_status(void *effect){
 uint32_t status=__atomic_load_n(&native_supported,__ATOMIC_ACQUIRE)?RLO_TRACK_SUPPORTED:0;
 if(!__atomic_load_n(&native_enabled,__ATOMIC_ACQUIRE))return status;
 RloTrackSnapshot view;if(!snapshot_read(&view)||!view.project||now_ms()-view.stamp>250)return status;
 status|=RLO_TRACK_READY|RLO_TRACK_MONITOR_ROUTED;int owns=0;
 for(unsigned i=0;i<view.count&&i<MAX_TRACK_FX;i++)if(view.fx[i]==(uintptr_t)effect)owns=1;
 /* Project lives until application shutdown. Never dereference a cached Track
    on the audio thread. Immutable snapshots avoid false capture stops while the
    UI publishes a normal refresh of the same selection. */
 if(owns&&view.track==__atomic_load_n((uint32_t*)(uintptr_t)(view.project+0xa0),__ATOMIC_ACQUIRE)){
  status|=RLO_TRACK_SELECTED;if(!view.arm)status|=RLO_TRACK_DISARMED;
  if(view.monitor==MONITOR_IN)status|=RLO_TRACK_INPUT_READY;
 }
 return status;
}
__attribute__((visibility("default"))) void retrolooper_mpc_track_prepare(void *effect){
 __atomic_store_n(&want_fx,(uint32_t)(uintptr_t)effect,__ATOMIC_RELEASE);
 __atomic_fetch_add(&request_gen,1,__ATOMIC_RELEASE);
}
static void rlo_track_prepare_physical(void){
 if(!__atomic_load_n(&native_supported,__ATOMIC_ACQUIRE))return;
 RloTrackSnapshot view;
 if(snapshot_read(&view)&&view.project&&view.count==0&&now_ms()-view.stamp<100&&view.track==__atomic_load_n((uint32_t*)(uintptr_t)(view.project+0xa0),__ATOMIC_ACQUIRE))return;
 retrolooper_mpc_track_prepare(0);
 uint32_t request=__atomic_load_n(&request_gen,__ATOMIC_ACQUIRE),start=now_ms();
 if(syscall(SYS_gettid)==getpid()){ui_refresh();return;}
 /* Delay delivery of the original REC/OVERDUB bytes until UI disarming has
    completed, bounded to 80 ms. No byte is changed, suppressed or remapped. */
 while((int32_t)(__atomic_load_n(&done_gen,__ATOMIC_ACQUIRE)-request)<0&&now_ms()-start<80){
  struct timespec pause={0,1000000};nanosleep(&pause,0);
 }
}


#else
static void rlo_track_start(void){}
static void rlo_track_pause(void){}
static void rlo_track_prepare_physical(void){}
__attribute__((visibility("default"))) uint32_t retrolooper_mpc_track_status(void *effect){(void)effect;return 0;}
__attribute__((visibility("default"))) void retrolooper_mpc_track_prepare(void *effect){(void)effect;}
#endif
