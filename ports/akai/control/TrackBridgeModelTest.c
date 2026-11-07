#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#define RLO_TRACK_BRIDGE_TEST 1
#define RLO_TRACK_STATE_PATH "/tmp/retrolooper-track-model-test.json"
#include <assert.h>
static void test_bool_set(void*,unsigned char);
static void test_monitor_set(void*,unsigned);
#include "TrackBridge.h"
_Static_assert(MONITOR_OFF==0&&MONITOR_IN==1&&MONITOR_AUTO==2&&MONITOR_MERGE==3,"Pinned native AudioProgram monitor values");
#define SAY(s) dprintf(2,"%s\n",s)
static uint32_t project[800],pool[128],poolTracks[2],track[512],other[512],program[4096],program2[4096];
static uint32_t chain[128],effect[64],wrapper[64],fx[64],slots[4],chain2[128],effect2[64],wrapper2[64],fx2[64],slots2[4];
static int filterCalls,setCalls,monitorCalls;static uint32_t monitorOrder[64],programOrder[64];
static void test_monitor_set(void *property,unsigned value){
 uintptr_t owner=(uintptr_t)property-0x26c8;assert(owner==(uintptr_t)program||owner==(uintptr_t)program2);
 assert(value<=MONITOR_MERGE);assert(monitorCalls<64);
 monitorOrder[monitorCalls]=value;programOrder[monitorCalls++]=owner;((uint32_t*)owner)[0x26f0/4]=value;
}
static unsigned char filter(void *data,const unsigned char *value){assert(data==(unsigned char*)track+0x484);assert(*value==0);++filterCalls;return 0;}
static void test_bool_set(void *property,unsigned char value){assert(property==(unsigned char*)track+0x474);assert(value==0);((unsigned char*)track)[0x483]=value;++setCalls;}
static void put(uint32_t *p,unsigned offset,uint32_t v){p[offset/4]=v;}
#ifdef RLO_MPC_TIMELINE_TEST
static uint32_t sequence[400],entry[7],arrangement[256],insertions,loads;
int rlo_mpc_insert_audio(uintptr_t base,uint32_t engine,const RloMpcTimelineTarget *target,const char *path,int start_load){
 assert(!base&&engine==1&&target->project==(uintptr_t)project&&target->track==(uintptr_t)track
        &&target->sequence==(uintptr_t)sequence&&target->arrangement==(uintptr_t)arrangement);
 assert(path[0]=='/'&&strstr(path,".wav"));
 if(start_load){++loads;return RLO_TIMELINE_LOADING;}
 ++insertions;
 return RLO_TIMELINE_BOUNCED;
}
static void test_timeline(void){
 put(project,0xe4,(uintptr_t)sequence);put(sequence,0,0x6d50cc4);
 put(sequence,0x528,(uintptr_t)entry);entry[4]=(uintptr_t)track;entry[5]=(uintptr_t)arrangement;
 put(arrangement,0,0x6d4a0f0);ui_refresh();
 RloMpcTimelineTarget target;
 assert(retrolooper_mpc_timeline_target(fx,&target)==RLO_TIMELINE_BOUNCED);
 assert(retrolooper_mpc_timeline_target(fx2,&target)==RLO_TIMELINE_CHANGED);
 const char *path="/tmp/retrolooper-timeline-model.wav";
 unlink(path);int fd=open(path,O_CREAT|O_EXCL|O_WRONLY,0600);assert(fd>=0);
 const uint8_t wave[]={ 'R','I','F','F',42,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
    0x80,0xbb,0,0,0x00,0x65,0x04,0,6,0,24,0,'d','a','t','a',6,0,0,0,0,0,0,0,0,0 };
 assert(write(fd,wave,sizeof wave)==sizeof wave);close(fd);
 uint32_t ticket,other_ticket;
 assert(retrolooper_mpc_timeline_enqueue(fx,&target,path,&ticket)==RLO_TIMELINE_PENDING);
 assert(retrolooper_mpc_timeline_enqueue(fx,&target,path,&other_ticket)==RLO_TIMELINE_BUSY);
 put(project,0xe4,0);ui_refresh();
 assert(retrolooper_mpc_timeline_result(ticket,0)==RLO_TIMELINE_CHANGED&&!insertions);
 put(project,0xe4,(uintptr_t)sequence);ui_refresh();
 assert(retrolooper_mpc_timeline_enqueue(fx,&target,path,&ticket)==RLO_TIMELINE_PENDING);
 put(project,0xa0,(uintptr_t)other);ui_refresh();
 assert(retrolooper_mpc_timeline_result(ticket,0)==RLO_TIMELINE_CHANGED&&!insertions);
 put(project,0xa0,(uintptr_t)track);ui_refresh();
 snapshots[snapshot_active].stamp-=150;
 RloMpcTimelineTarget stale;
 assert(retrolooper_mpc_timeline_target(fx,&stale)==RLO_TIMELINE_UNAVAILABLE);
 assert(retrolooper_mpc_timeline_enqueue(fx,&target,path,&ticket)==RLO_TIMELINE_PENDING);
 ui_refresh();assert(retrolooper_mpc_timeline_result(ticket,0)==RLO_TIMELINE_PENDING&&loads==1&&!insertions);
 ui_refresh();assert(retrolooper_mpc_timeline_result(ticket,0)==RLO_TIMELINE_BOUNCED&&insertions==1);
 assert(loads==1);
 unlink(path);
 SAY("PASS: MPC timeline WAV validation, stale worker snapshot, destination ownership, track and sequence changes, one sample load before placement");
}
#endif
/* Independent fixture from the live, unarmed Audio track: raw mode 1 passes
   input; raw mode 2 did not. Do not derive this expectation from our enum. */
static int unarmedInputFlows(uint32_t *program){unsigned raw=program[0x26f0/4];return raw==1||raw==3;}
int main(void){
 native_mem=open("/proc/self/mem",O_RDONLY|O_CLOEXEC);assert(native_mem>=0);native_project=(uintptr_t)project;
 put(project,0,0x6d501c4);put(project,0xa0,(uintptr_t)track);put(track,0,0x6d51624);
 put(project,0xb1c,(uintptr_t)pool);put(pool,0,0x6cb7528);poolTracks[0]=(uintptr_t)track;poolTracks[1]=(uintptr_t)other;
 put(pool,0x60,(uintptr_t)poolTracks);put(pool,0x64,(uintptr_t)(poolTracks+2));put(other,0,0x6d51624);
 put(track,0x648,(uintptr_t)program);put(track,0x64c,6);put(program,0,0x6d4c640);put(program,0x5e0,(uintptr_t)chain);
 put(chain,0,0x6cb6aa8);slots[0]=(uintptr_t)effect;put(chain,0x60,(uintptr_t)slots);put(chain,0x64,(uintptr_t)(slots+4));
 put(effect,0,0x6d4b5f8);put(effect,0x68,(uintptr_t)wrapper);put(wrapper,0,0x6c90c90);put(wrapper,0xcc,(uintptr_t)fx);
 fx[0]=0x56737450;fx[11]=(uintptr_t)wrapper;fx[18]=0x526c4f31;fx[19]=301;
 put(program,0x26e4,1);put(program2,0x26e4,1);
 /* Start with a saved MERGE on the inactive owner and IN on the selected owner. */
 put(program,0x26f0,MONITOR_IN);put(program2,0x26f0,MONITOR_MERGE);
 put(other,0x648,(uintptr_t)program2);put(other,0x64c,6);put(program2,0,0x6d4c640);put(program2,0x5e0,(uintptr_t)chain2);
 put(chain2,0,0x6cb6aa8);slots2[0]=(uintptr_t)effect2;put(chain2,0x60,(uintptr_t)slots2);put(chain2,0x64,(uintptr_t)(slots2+4));
 put(effect2,0,0x6d4b5f8);put(effect2,0x68,(uintptr_t)wrapper2);put(wrapper2,0,0x6c90c90);put(wrapper2,0xcc,(uintptr_t)fx2);
 fx2[0]=0x56737450;fx2[11]=(uintptr_t)wrapper2;fx2[18]=0x526c4f31;fx2[19]=304;
 uint32_t selected,armed,found[16];unsigned count;
 assert(selected_effects(&selected,&armed,found,&count)&&count==1&&found[0]==(uintptr_t)fx&&armed==0);
 put(track,0x48c,1);put(track,0x490,(uintptr_t)filter);((unsigned char*)track)[0x483]=1;
 native_disarm((uintptr_t)track);assert(filterCalls==1&&setCalls==1&&((unsigned char*)track)[0x483]==0);
 native_supported=1;native_enabled=1;ui_refresh();assert(retrolooper_mpc_track_status(fx)==63);
 assert(monitorCalls==2&&programOrder[0]==(uintptr_t)program2&&monitorOrder[0]==MONITOR_OFF
   &&programOrder[1]==(uintptr_t)program&&monitorOrder[1]==MONITOR_MERGE);
 assert(program[0x26f0/4]==3&&unarmedInputFlows(program)&&!unarmedInputFlows(program2));
 ui_refresh();assert(monitorCalls==2); /* Stable routing is idempotent. */
 put(project,0xa0,(uintptr_t)other);assert((retrolooper_mpc_track_status(fx)&RLO_TRACK_SELECTED)==0);
 ui_refresh();assert(retrolooper_mpc_track_status(fx2)==63&&native_word((uintptr_t)program+0x26f0)==MONITOR_OFF);
 assert(programOrder[2]==(uintptr_t)program&&monitorOrder[2]==MONITOR_OFF&&programOrder[3]==(uintptr_t)program2&&monitorOrder[3]==MONITOR_MERGE);
 put(program2,0x26f0,MONITOR_OFF);ui_refresh();assert(retrolooper_mpc_track_status(fx2)==31); /* Respect a stable selected-track manual OFF. */
 retrolooper_mpc_track_prepare(fx2);ui_refresh();assert(retrolooper_mpc_track_status(fx2)==63); /* Capture cue prepares input again. */
 assert(program2[0x26f0/4]==3&&unarmedInputFlows(program2)); /* Cue must keep live input, not select raw AUTO=2. */
 put(program2,0x26f0,2);ui_refresh();assert(retrolooper_mpc_track_status(fx2)==31&&!unarmedInputFlows(program2));
 retrolooper_mpc_track_prepare(fx2);ui_refresh();
 assert(retrolooper_mpc_track_status(fx2)==63&&program2[0x26f0/4]==3&&unarmedInputFlows(program2));
 int manualCalls=monitorCalls;
 put(program2,0x26f0,MONITOR_IN);ui_refresh();
 assert(retrolooper_mpc_track_status(fx2)==63&&monitorCalls==manualCalls); /* Stable manual IN still passes input. */
 retrolooper_mpc_track_prepare(fx2);ui_refresh();
 assert(retrolooper_mpc_track_status(fx2)==63&&monitorCalls==manualCalls+1&&native_word((uintptr_t)program2+0x26f0)==MONITOR_MERGE);
 for(unsigned invalid=4;invalid;invalid=invalid==4?UINT32_MAX:0){
  put(program,0x26f0,invalid);int before=monitorCalls;ui_refresh();
  assert(retrolooper_mpc_track_status(fx2)==RLO_TRACK_SUPPORTED&&monitorCalls==before); /* No partial writes on unknown modes. */
 }
 put(program,0x26f0,MONITOR_OFF);ui_refresh();assert(retrolooper_mpc_track_status(fx2)==63);
 put(pool,0x64,(uintptr_t)poolTracks+MAX_NATIVE_TRACKS*4+4);int calls=monitorCalls;assert(!route_native_monitors((uintptr_t)other,0)&&monitorCalls==calls);put(pool,0x64,(uintptr_t)(poolTracks+2));
 put(program,0x26e4,0);assert(!route_native_monitors((uintptr_t)other,0)&&monitorCalls==calls);put(program,0x26e4,1);
 put(project,0xa0,(uintptr_t)track);snapshots[snapshot_active].stamp=now_ms()-251;assert(retrolooper_mpc_track_status(fx)==RLO_TRACK_SUPPORTED);
 ui_refresh();assert(retrolooper_mpc_track_status(other)==(RLO_TRACK_SUPPORTED|RLO_TRACK_READY|RLO_TRACK_MONITOR_ROUTED));
 fx[11]=0;assert(!selected_effects(&selected,&armed,found,&count));fx[11]=(uintptr_t)wrapper;
 put(chain,0x64,(uintptr_t)slots+68);assert(!selected_effects(&selected,&armed,found,&count));put(chain,0x64,(uintptr_t)(slots+4));
 put(track,0x64c,3);assert(selected_effects(&selected,&armed,found,&count)&&count==0);put(track,0x64c,6);
 // Native queue move/destruction must not release a still-pending callback.
 unsigned char a[144]={1},b[144]={0};ui_pending=1;ui_manage(0,a,b);
 assert(a[0]==0&&b[0]==1);ui_manage(1,a,0);assert(ui_pending==1);ui_manage(1,b,0);assert(ui_pending==0);
 put(project,0xa0,(uintptr_t)track);ui_refresh();unsigned busy=snapshot_active^1;snapshot_readers[busy]=1;
 ui_refresh();assert(retrolooper_mpc_track_status(fx)==63);snapshot_readers[busy]=0;
#ifdef RLO_MPC_TIMELINE_TEST
 test_timeline();
#endif
 rlo_track_pause();assert(retrolooper_mpc_track_status(fx)==RLO_TRACK_SUPPORTED);
 close(native_mem);unlink(RLO_TRACK_STATE_PATH);
 SAY("PASS: ARM live-owner graph, native arm payload, raw MERGE=3 default and capture preparation, AUTO=2 rejection until prepared, saved MERGE and manual IN routing, invalid-mode rejection without writes, selected-only monitor routing/order/idempotence, bounded live track-pool ownership, mutex validation, stale snapshots and native UI callback lifetime");
 return 0;
}

