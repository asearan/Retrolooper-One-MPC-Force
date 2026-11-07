#define RLO_FORCE_CLIP_MODEL_TEST 1
#define main legacy_editor_model_main
#include "EditorTrackModelTest.c"
#undef main
static uint32_t effects_manager[32],queues[64];
static uint32_t clip_lanes[32],clip_rows[3][34],clip_node[7],playhead[32];
static unsigned imports;
int rlo_force_load_audio_file(uintptr_t base,uint32_t target_engine,const char *path){
 assert(base==native_base&&target_engine==(uintptr_t)engine&&access(path,R_OK)==0);
 assert(playhead[0x44/4]==1);
 ++imports;
 clip_node[4]=native_word((uintptr_t)project+0xa0);
 clip_node[5]=100+imports;clip_node[6]=200+imports;
 put(clip_rows[1],0x78,(uintptr_t)clip_node);
 return 1;
}
static void test_live_bounce(void){
 native_application=(uintptr_t)app;
 assert(force_engine()==(uintptr_t)engine);
 put(engine,0x274,(uintptr_t)playhead);
 put(playhead,0,native_base+0x6c9be2c);put(playhead,0x44,1);
 put(project,0x1b4,(uintptr_t)track[0]);put(project,0x1b8,1);
 put(project,0xb20,(uintptr_t)clip_lanes);
 put(clip_lanes,0x60,(uintptr_t)clip_rows);put(clip_lanes,0x64,(uintptr_t)(clip_rows+3));
 char folder[]="/tmp/retrolooper-force-bounce.XXXXXX",path[128];
 assert(mkdtemp(folder));
 snprintf(path,sizeof path,"%s/test.wav",folder);
 const unsigned char wav[]={
  'R','I','F','F',42,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
  0x44,0xac,0,0,0x98,0x09,0x04,0,6,0,24,0,'d','a','t','a',6,0,0,0,0,0,0,0,0,0
 };
 int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);
 assert(fd>=0&&write(fd,wav,sizeof wav)==sizeof wav&&close(fd)==0);
 for(unsigned pass=0;pass<2;++pass){
  RloForceClipTarget target;uint32_t ticket=0;
  ui_refresh();
  assert(retrolooper_force_clip_target(fx[0],&target)==RLO_CLIP_BOUNCED);
  assert(target.row==1&&target.clip==(pass?101:0));
  assert(retrolooper_force_clip_enqueue(fx[0],&target,path,&ticket)==RLO_CLIP_PENDING);
  assert(imports==pass);
  ui_refresh();
  assert(retrolooper_force_clip_result(ticket,0)==RLO_CLIP_BOUNCED);
  assert(imports==pass+1&&playhead[0x44/4]==1);
 }
 RloForceClipTarget target;uint32_t ticket=0;
 ui_refresh();
 assert(retrolooper_force_clip_target(fx[0],&target)==RLO_CLIP_BOUNCED);
 assert(retrolooper_force_clip_enqueue(fx[0],&target,path,&ticket)==RLO_CLIP_PENDING);
 put(project,0x1b8,2);ui_refresh();
 assert(retrolooper_force_clip_result(ticket,0)==RLO_CLIP_SELECTION_CHANGED);
 assert(imports==2);
 /* A picked slot may be on another Audio track. The picker retains its
    source insert even after native selection moves to the destination. */
 put(project,0xa0,(uintptr_t)track[1]);put(project,0x1b4,(uintptr_t)track[1]);put(project,0x1b8,1);
 assert(force_clip_target((uintptr_t)track[1],&target)&&target.clip==0);
 force_picker.effect=(uintptr_t)fx[0];force_picker.token=1;force_picker.project=(uintptr_t)project;
 force_picker.source=(uintptr_t)track[0];force_picker.started=now_ms();force_picker.target=target;
 assert(retrolooper_force_clip_enqueue(fx[0],&target,path,&ticket)==RLO_CLIP_PENDING);
 ui_refresh();
 assert(retrolooper_force_clip_result(ticket,0)==RLO_CLIP_BOUNCED&&imports==3&&playhead[0x44/4]==1);
 assert(force_clip_target((uintptr_t)track[1],&target)&&target.clip==103);
 put(project,0xa0,(uintptr_t)track[0]);
 assert(unlink(path)==0&&rmdir(folder)==0);
 put(project,0x1b4,0);put(project,0x1b8,0);put(project,0xb20,0);
 native_application=0;
 dprintf(2,"PASS: Force playing-state clip selection, queued insert and replacement, changed-slot rejection and cross-track bounce\n");
}
static unsigned opens,closes;
static void open_editor(uint32_t manager,uint32_t iface,uint32_t slot,uint32_t native_mode){
 assert(manager==(uintptr_t)effects_manager&&native_mode==(uintptr_t)mode);
 unsigned pg=iface==(uintptr_t)program[0]+0x3c?0:1;assert(iface==(uintptr_t)program[pg]+0x3c);
 assert(native_word((uintptr_t)project+0xa0)==(uintptr_t)track[pg]);
 ++opens;show(pg,slot);
}
static void close_editor(uint32_t native_mode,unsigned option,unsigned popup){assert(native_mode==(uintptr_t)mode&&option==0&&popup==0x30fa);++closes;put(mode,0x6c,0x3080);}
static void select_clip(uint32_t p,uint32_t t,uint32_t slot,unsigned arrange){
 assert(p==(uintptr_t)project);
 if(arrange)assert(t==(uintptr_t)track[0]&&slot==101);
 else assert(t==(uintptr_t)track[1]&&slot==1);
 put(project,0xa0,t);put(project,0x1b4,t);put(project,0x1b8,slot);
 put(project,0x1bc,arrange);
}
static void begin(void){
 native_mem=open("/proc/self/mem",O_RDONLY|O_CLOEXEC);assert(native_mem>=0);native_supported=native_enabled=1;
 native_queues=(uintptr_t)queues;test_editor_application=(uintptr_t)app;test_editor_select=select_track;
 memset(&last_editor_owner,0,sizeof last_editor_owner);force_last_selected=force_follow_target=force_follow_effect=force_clip_editor_selection=0;
 opens=closes=selections=force_editor_switches=force_editor_closes=0;visible_editor_fx=visible_editor_open=0;
 put(project,0x94,1);put(project,0xa0,(uintptr_t)track[0]);put(pool,0x64,(uintptr_t)(tracks+2));
 put(track[0],0x64c,6);put(track[1],0x64c,6);put(program[0],0x3c,0x6d4b81c);put(program[1],0x3c,0x6d4b81c);
 test_force_editor_open=open_editor;test_force_editor_close=close_editor;
 put(effects_manager,0,0x6cb6dfc);put(effects_manager,0xc,native_queues);put(effects_manager,0x18,(uintptr_t)session);
 put(editor,0x1f8,(uintptr_t)mode);put(editor,0x1fc,(uintptr_t)effects_manager);
 put(mode,0x40,0x2083);((unsigned char*)editor)[8+0x68]=2;show(0,0);ui_refresh();pad_refresh_context();
 assert(visible_editor_fx==(uintptr_t)fx[0]&&pad_owner(fx[0])&&selections==0);
}
static void test_picker_workflows(void){
 static uint32_t controller[1],parent[32],viewport[192],split[8],matrix_lanes[128],columns[3];
 uint32_t *view=viewport+70,*behaviour=parent+5;
 native_application=(uintptr_t)app;test_force_picker_select=select_clip;
 put(engine,0x7b8,(uintptr_t)controller);controller[0]=(uintptr_t)parent;
 put(behaviour,0,0x6ca8428);put(behaviour,0x40,(uintptr_t)view);put(behaviour,0x48,(uintptr_t)project);
 put(viewport,0x28,1);put(view,0,0x6c9a9b0);put(view,0x1b4,(uintptr_t)split);
 put(view,0x178,(uintptr_t)matrix_lanes);put(matrix_lanes,0,0x6c9bfe0);put(matrix_lanes,0x48,3);
 put(matrix_lanes,0x184,(uintptr_t)project);put(matrix_lanes,0x1bc,(uintptr_t)columns);
 columns[1]=(uintptr_t)tracks;columns[2]=(uintptr_t)(tracks+2);
 put(project,0xb20,(uintptr_t)clip_lanes);
 put(clip_lanes,0x60,(uintptr_t)clip_rows);put(clip_lanes,0x64,(uintptr_t)(clip_rows+3));
 assert(retrolooper_force_picker_begin(fx[0],17)==RLO_CLIP_PENDING);
 assert(force_picker_pad(0,1,1)&&force_picker.pad==2);
 ui_refresh();
 RloForceClipTarget target;
 assert(retrolooper_force_picker_poll(fx[0],17,&target,0)==RLO_CLIP_BOUNCED);
 assert(target.track==(uintptr_t)track[1]&&target.row==1);
 assert(native_word(native_engine_mode+0x40)==0x2083&&visible_editor_open&&visible_editor_fx==(uintptr_t)fx[0]&&!opens&&!closes);
 retrolooper_force_picker_poll(fx[0],17,0,1);
 assert(retrolooper_force_picker_begin(fx[0],19)==RLO_CLIP_PENDING&&force_picker.source==(uintptr_t)track[0]);
 retrolooper_force_picker_poll(fx[0],19,0,1);

 put(project,0xa0,(uintptr_t)track[0]);ui_refresh();
 put(project,0xe4,101);put(project,0x1bc,0);put(project,0x1b4,(uintptr_t)track[0]);put(project,0x1b8,1);
 put(project,0xb20,(uintptr_t)clip_lanes);
 put(clip_lanes,0x60,(uintptr_t)clip_rows);put(clip_lanes,0x64,(uintptr_t)(clip_rows+3));
 assert(retrolooper_force_picker_begin(fx[0],18)==RLO_CLIP_PENDING);
 put(mode,0x40,FORCE_MATRIX_MODE);put(mode,0x6c,FORCE_NO_POPUP);ui_refresh();
 assert(force_picker.matrix_ready&&native_word((uintptr_t)project+0x1bc)==1);
 assert(retrolooper_force_picker_poll(fx[0],18,0,0)==RLO_CLIP_PENDING);
 put(project,0x1bc,0);ui_refresh();
 put(project,0x1b8,1);ui_refresh();
 assert(retrolooper_force_picker_poll(fx[0],18,&target,0)==RLO_CLIP_BOUNCED);
 assert(target.track==(uintptr_t)track[0]&&target.row==1);
 retrolooper_force_picker_poll(fx[0],18,0,1);
 put(project,0xe4,0);put(project,0x1b4,0);put(project,0x1b8,0);put(project,0xb20,0);
 native_application=0;
 dprintf(2,"PASS: Force pad selection keeps the looper open across tracks and Matrix touchscreen selection remains available\n");
}
int main(void){
 assert(legacy_editor_model_main()==0);begin();
 test_picker_workflows();close(native_mem);begin();
 test_live_bounce();
 put(project,0xa0,(uintptr_t)track[1]);ui_refresh();pad_refresh_context();
 assert(opens==1&&selections==0&&visible_editor_fx==(uintptr_t)fx[1]&&pad_owner(fx[1])&&!pad_owner(fx[0]));
 assert(native_word((uintptr_t)program[0]+0x26f0)==0&&native_word((uintptr_t)program[1]+0x26f0)==MONITOR_MERGE);
 for(unsigned i=0;i<8;++i)ui_refresh();assert(opens==1&&selections==0);
 for(unsigned i=0;i<10;++i){unsigned n=i%2;put(project,0xa0,(uintptr_t)track[n]);ui_refresh();pad_refresh_context();assert(visible_editor_fx==(uintptr_t)fx[n]&&pad_owner(fx[n])&&selections==0);}
 assert(opens==11);
 /* Explicitly opening another instance retains owner-selection behaviour. */
 show(0,0);ui_refresh();assert(selections==1&&native_word((uintptr_t)project+0xa0)==(uintptr_t)track[0]);
 /* A second insert is retained by slot where available; fallback is first. */
 show(1,1);ui_refresh();assert(selections==2&&visible_editor_fx==(uintptr_t)fx[2]);
 put(project,0xa0,(uintptr_t)track[0]);ui_refresh();assert(visible_editor_fx==(uintptr_t)fx[0]);
 put(project,0xa0,(uintptr_t)track[1]);ui_refresh();assert(visible_editor_fx==(uintptr_t)fx[1]);
 /* Selecting a track without this looper closes the editor; pads return native. */
 tracks[2]=(uintptr_t)track[2];put(track[2],0,0x6d50644);put(track[2],0x64c,3);put(pool,0x64,(uintptr_t)(tracks+3));
 put(project,0xa0,(uintptr_t)track[2]);ui_refresh();pad_refresh_context();assert(closes==1&&!visible_editor_open&&!visible_editor_fx&&!pad_owner(fx[0])&&!pad_owner(fx[1]));
 close(native_mem);begin();
 /* Broken links and deleted/ambiguous owners never call native code. */
 put(effects_manager,0x18,0);put(project,0xa0,(uintptr_t)track[1]);ui_refresh();assert(opens==0&&selections==0&&!visible_editor_fx);put(effects_manager,0x18,(uintptr_t)session);
 put(project,0xa0,(uintptr_t)track[0]);ui_refresh();put(pool,0x64,(uintptr_t)(tracks+1));put(project,0xa0,(uintptr_t)track[1]);ui_refresh();assert(opens==0&&!visible_editor_fx);put(pool,0x64,(uintptr_t)(tracks+2));
 put(project,0xa0,(uintptr_t)track[0]);ui_refresh();put(mode,0x40,0x208d);put(project,0xa0,(uintptr_t)track[1]);ui_refresh();assert(opens==0&&!visible_editor_fx);
 close(native_mem);unlink(RLO_TRACK_STATE_PATH);
 dprintf(2,"PASS: Force track buttons retarget the visible looper and pad owner, no selection fights or repeated opens, rapid switches, native monitor handoff, explicit editor owner selection, second-slot fallback, no-looper close, invalid manager/deleted owner rejection and native-mode protection\n");return 0;
}
