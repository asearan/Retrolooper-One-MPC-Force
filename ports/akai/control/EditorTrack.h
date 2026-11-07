#pragma once
/* Pinned MPC 3.10.0.49 UI/model paths. Called only from our native UI queue.
   The live overlay child list supplies visibility. EditEffectSession supplies
   its AudioProgram interface and insert slot; the Project TrackPool supplies
   ownership. Parameter polling and remembered editor focus are not used. */
typedef struct {uint32_t component,session,track,program,effect;} RloEditorOwner;
static RloEditorOwner last_editor_owner;
static uint32_t visible_editor_fx,visible_editor_open,editor_selection_count;
#ifdef RLO_TRACK_BRIDGE_TEST
static uint32_t test_editor_application;
static void (*test_editor_select)(void*,uint32_t);
#endif
static int editor_fingerprint(void){
#if defined(RLO_TRACK_BRIDGE_TEST) && !defined(RLO_EDITOR_READONLY_PROBE)
 return 1;
#else
 return word_is(native_base+0x100e490,0xe5953028) /* notifying pointer property */
   &&word_is(native_base+0x28fcac0,0xe2800078) /* native SelectTrack callback */
   &&word_is(native_base+0x28fcac4,0xea9c4659)
   &&word_is(native_base+0xb5bb40,0xe5900030) /* JUCE child count */
   &&word_is(native_base+0xb5bb50,0x35903028)
   &&word_is(native_base+0x1ea2084,0xe5900034) /* EditEffectSession's Program */
   &&word_is(native_base+0x1ea2094,0xe5931038);
#endif
}
static int editor_owner(RloEditorOwner *out){
 memset(out,0,sizeof *out);
 if(!editor_fingerprint()||!word_is(native_engine_mode,native_base+0x6ca3344))return 0;
 if(native_word(native_engine_mode+0x6c)!=0x30fa)return 0;
 uint32_t app=0;
#ifdef RLO_TRACK_BRIDGE_TEST
 app=test_editor_application;
#else
 app=native_word(native_base+0x6f35c88);
#endif
 if(!word_is(app,native_base+0x6cdfaf4))return 0;
 uint32_t window=native_word(app+0xc4);
 if(!word_is(window,native_base+0x6cdfe94))return 0;
 uint32_t engine=native_word(window+0x26c);
 if(!word_is(engine,native_base+0x6ce5650)||native_word(engine+0x264)!=native_project||native_word(engine+0x118)!=native_engine_mode)return 0;
 uint32_t touch=native_word(window+0x274);
 /* StandaloneFilterWindow holds the touchscreen's Component interface,
    four bytes after TouchMpcInterface's primary base. */
 if(touch<4||!word_is(touch,native_base+0x6dafe14)||!word_is(touch-4,native_base+0x6dafdd0))return 0;
 uint32_t container=native_word(touch+0xa0);
 if(!word_is(container,native_base+0x6dd3d3c)||!word_is(container+0x10,native_base+0x6dd3de4))return 0;
 uint32_t array=native_word(container+0x38),count=native_word(container+0x40),component=0;
 if(!array||count>128)return 0;
 for(unsigned i=0;i<count;++i){uint32_t child=native_word(array+4*i);unsigned char flags=0;
  if(child<8||!word_is(child,native_base+0x6ddacac)||!word_is(child-8,native_base+0x6ddac38))continue;
  /* OverlayContainer holds Component pointers, eight bytes after the
     insert editor's primary base. Read the native visibility flag there. */
  if(!native_read(&flags,child+0x68,1)||!(flags&2)||native_word(child+0xc)!=container+0x10)continue;
  if(component)return 0;component=child-8;
 }
 if(!component)return 0;
 uint32_t props=native_word(component+0x1ac),session=native_word(props+0x5c);
 if(!word_is(session,native_base+0x6ca95a0))return 0;
 uint32_t iface=native_word(session+0x34),slot=native_word(session+0x38);
 if(iface<0x3c||slot>=MAX_TRACK_FX||!word_is(iface,native_base+0x6d4c7fc))return 0;
 uint32_t program=iface-0x3c;
 if(!word_is(program,native_base+0x6d4c640))return 0;
 uint32_t inserts=native_word(program+0x5e0);
 if(!word_is(inserts,native_base+0x6cb6aa8))return 0;
 uint32_t begin=native_word(inserts+0x60),end=native_word(inserts+0x64);
 if(end<begin||(end-begin)%4||end-begin>MAX_TRACK_FX*4||slot>=(end-begin)/4)return 0;
 uint32_t insert=native_word(begin+slot*4);
 if(!word_is(insert,native_base+0x6d4b5f8))return 0;
 uint32_t wrapper=native_word(insert+0x68),words[20];
 if(!word_is(wrapper,native_base+0x6c90c90))return 0;
 uint32_t effect=native_word(wrapper+0xcc);
 if(!native_read(words,effect,sizeof words)||words[0]!=0x56737450||words[11]!=wrapper||words[18]!=0x526c4f31)return 0;
 if(!word_is(native_project,native_base+0x6d501c4))return 0;
 uint32_t pool=native_word(native_project+0xb1c);
 if(!word_is(pool,native_base+0x6cb7528))return 0;
 begin=native_word(pool+0x60);end=native_word(pool+0x64);
 if(end<begin||(end-begin)%4||end-begin>MAX_NATIVE_TRACKS*4)return 0;
 uint32_t owner=0;
 for(uint32_t at=begin;at<end;at+=4){uint32_t track=native_word(at);
  if(!track)continue;if(!word_is(track,native_base+0x6d51624))return 0;
  if(native_word(track+0x64c)!=6||native_word(track+0x648)!=program)continue;
  uint32_t pg=0,arm=0,fx[MAX_TRACK_FX];unsigned n=0;int owns=0;
  if(!track_effects(track,&pg,&arm,fx,&n))return 0;
  for(unsigned j=0;j<n;++j)if(fx[j]==effect)owns=1;
  if(!owns||owner)return 0;owner=track;
 }
 if(!owner)return 0;
 *out=(RloEditorOwner){component,session,owner,program,effect};return 1;
}
static void follow_editor_track(void){
 RloEditorOwner current;visible_editor_fx=0;
 visible_editor_open=word_is(native_engine_mode,native_base+0x6ca3344)&&native_word(native_engine_mode+0x6c)==0x30fa;
 if(!editor_owner(&current)){memset(&last_editor_owner,0,sizeof last_editor_owner);return;}
 visible_editor_fx=current.effect;
 if(!memcmp(&current,&last_editor_owner,sizeof current))return;
 last_editor_owner=current;
 if(native_word(native_project+0xa0)==current.track)return;
 /* Project::SelectTrack's normal UI callback calls this exact recursive-
    mutex, notifying setter on Project+0x78. The Track is still owned by the
    live TrackPool on this UI thread. Do not assign Project+0xa0 directly. */
 if(native_word(native_project+0x94)!=1)return;
#ifdef RLO_TRACK_BRIDGE_TEST
 if(!test_editor_select)return;test_editor_select((void*)(native_project+0x78),current.track);
#else
 ((void(*)(void*,uint32_t))(native_base+0x100e430))((void*)(native_project+0x78),current.track);
#endif
 if(native_word(native_project+0xa0)==current.track)++editor_selection_count;
}
