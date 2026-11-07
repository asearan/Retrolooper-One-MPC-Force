#pragma once
#include "ClipTarget.h"

/* Only ui_refresh invokes the native loader. A queued request carries its
   original slot, with an additional check after the WAV render completes. */
enum { FORCE_BOUNCE_PATH=512 };
static pthread_mutex_t force_bounce_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct {
 RloForceClipTarget target;
 uint32_t id,effect,submitted,source;
 int running,result,abandoned;
 char path[FORCE_BOUNCE_PATH];
} force_bounce;
static uint32_t force_bounce_serial;
#include "ClipPicker.h"

#if !defined(RLO_TRACK_BRIDGE_TEST) || defined(RLO_FORCE_CLIP_MODEL_TEST)
extern int rlo_force_load_audio_file(uintptr_t,uint32_t,const char*);
#endif
static int force_bounce_fingerprint(void){
#if defined(RLO_FORCE_CLIP_MODEL_TEST)
 return 1;
#elif defined(RLO_TRACK_BRIDGE_TEST) && !defined(RLO_EDITOR_READONLY_PROBE)
 return 0;
#else
 return word_is(native_base+0x983e60,0xe16d41f0)
     &&word_is(native_base+0x934b88,0xe5903000)
     &&word_is(native_base+0x22dc35c,0xe59f21e0)
     &&word_is(native_base+0x1dab020,0xebfffe03)
     &&word_is(native_base+0x1daafd8,0xe59fc0b4)
     &&word_is(native_base+0x1daaa70,0xebfff4dc)
     &&word_is(native_base+0x28f1930,0xe3500000)
     &&word_is(native_base+0x28f19d4,0xf46e078f);
#endif
}
static int force_bounce_matches(uint32_t track,const RloForceClipTarget *target){
 RloForceClipTarget actual;
 return force_clip_target(track,&actual)&&!memcmp(&actual,target,sizeof actual);
}
static int force_bounce_apply(uint32_t track,const RloForceClipTarget *target,const char *path){
 uint32_t engine=force_engine();
 if(!engine||!force_bounce_fingerprint())return RLO_CLIP_UNAVAILABLE;
 if(!force_bounce_matches(track,target))return RLO_CLIP_SELECTION_CHANGED;
#if defined(RLO_TRACK_BRIDGE_TEST) && !defined(RLO_FORCE_CLIP_MODEL_TEST)
 (void)path;
 return RLO_CLIP_UNAVAILABLE;
#else
 int loaded=rlo_force_load_audio_file(native_base,engine,path);
 uint32_t clip,owner;
 int changed=force_clip_slot(target->lanes,track,target->row,&clip,&owner)&&clip&&owner&&clip!=target->clip;
 dprintf(STDERR_FILENO,"Retrolooper Force clip: row %u, native import %d, slot changed %d\n",target->row,loaded,changed);
 return loaded&&changed?RLO_CLIP_BOUNCED:RLO_CLIP_IMPORT_FAILED;
#endif
}
static void force_bounce_refresh(uint32_t track,const uint32_t *fx,unsigned count){
 if(pthread_mutex_trylock(&force_bounce_mutex))return;
 if(!force_bounce.id||force_bounce.running||force_bounce.result){pthread_mutex_unlock(&force_bounce_mutex);return;}
 unsigned i=0;
 while(i<count&&fx[i]!=force_bounce.effect)++i;
 int owns=force_bounce.source
    ? force_source_owns(force_bounce.source,force_bounce.effect) : i<count;
 if(!owns||now_ms()-force_bounce.submitted>3000){
  force_bounce.result=RLO_CLIP_SELECTION_CHANGED;
  pthread_mutex_unlock(&force_bounce_mutex);
  return;
 }
 force_bounce.running=1;
 pthread_mutex_unlock(&force_bounce_mutex);
 int result=force_bounce_apply(track,&force_bounce.target,force_bounce.path);
 pthread_mutex_lock(&force_bounce_mutex);
 force_bounce.running=0;
 force_bounce.result=result;
 if(force_bounce.abandoned)force_bounce.id=0;
 pthread_mutex_unlock(&force_bounce_mutex);
}
__attribute__((visibility("default"))) int retrolooper_force_clip_target(void *effect,RloForceClipTarget *out){
 RloTrackSnapshot view;
 if(!out||!effect||!snapshot_read(&view)||now_ms()-view.stamp>100)return RLO_CLIP_UNAVAILABLE;
 for(unsigned i=0;i<view.count;++i)if(view.fx[i]==(uintptr_t)effect){
  *out=view.clip_target;
  return view.clip_status?view.clip_status:RLO_CLIP_UNAVAILABLE;
 }
 return RLO_CLIP_SELECTION_CHANGED;
}
__attribute__((visibility("default"))) int retrolooper_force_clip_enqueue(void *effect,const RloForceClipTarget *target,const char *path,uint32_t *id){
 if(!target||!path||!id)return RLO_CLIP_UNAVAILABLE;
 RloForceClipTarget current;
 size_t len=strnlen(path,FORCE_BOUNCE_PATH);
 if(len<5||len==FORCE_BOUNCE_PATH||path[0]!='/'||strcmp(path+len-4,".wav"))return RLO_CLIP_IMPORT_FAILED;
 int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
 if(fd<0)return RLO_CLIP_IMPORT_FAILED;
 // Force's 32-bit fstat entry point rejects regular files with EINVAL on
 // internal storage. Use the verified large-file ABI explicitly.
 struct stat64 st;
 uint8_t wav[44];
 uint32_t riff=0,rate=0,data=0;
 int valid=!fstat64(fd,&st)&&S_ISREG(st.st_mode)&&read(fd,wav,sizeof wav)==sizeof wav;
 close(fd);
 if(!valid||memcmp(wav,"RIFF",4)||memcmp(wav+8,"WAVEfmt ",8)||memcmp(wav+16,"\020\0\0\0\001\0\002\0",8)
    ||memcmp(wav+32,"\006\0\030\0data",8))return RLO_CLIP_IMPORT_FAILED;
 memcpy(&riff,wav+4,4);memcpy(&rate,wav+24,4);memcpy(&data,wav+40,4);
 if((rate!=44100&&rate!=48000)||!data||data%6||data>rate*120u*6u||st.st_size!=data+44u||riff!=data+36u)return RLO_CLIP_IMPORT_FAILED;
 pthread_mutex_lock(&force_bounce_mutex);
 if(force_bounce.id){pthread_mutex_unlock(&force_bounce_mutex);return RLO_CLIP_BUSY;}
 uint32_t source=0;
 if(force_picker.effect==(uintptr_t)effect&&force_picker.target.project&&
    !memcmp(&force_picker.target,target,sizeof current)){
  source=force_picker.source;
  memset(&force_picker,0,sizeof force_picker);
 }else{
  int status=retrolooper_force_clip_target(effect,&current);
  if(status!=RLO_CLIP_BOUNCED||memcmp(&current,target,sizeof current)){
   pthread_mutex_unlock(&force_bounce_mutex);
   return status==RLO_CLIP_BOUNCED?RLO_CLIP_SELECTION_CHANGED:status;
  }
 }
 if(!++force_bounce_serial)++force_bounce_serial;
 *id=force_bounce.id=force_bounce_serial;
 force_bounce.target=*target;
 force_bounce.effect=(uintptr_t)effect;
 force_bounce.source=source;
 force_bounce.submitted=now_ms();
 force_bounce.running=force_bounce.result=force_bounce.abandoned=0;
 memcpy(force_bounce.path,path,len+1);
 pthread_mutex_unlock(&force_bounce_mutex);
 return RLO_CLIP_PENDING;
}
__attribute__((visibility("default"))) int retrolooper_force_clip_result(uint32_t id,int cancel){
 pthread_mutex_lock(&force_bounce_mutex);
 int result=RLO_CLIP_UNAVAILABLE;
 if(id&&id==force_bounce.id){
  result=force_bounce.result;
  if(cancel&&force_bounce.running)force_bounce.abandoned=1;
  else if(cancel||result)force_bounce.id=0;
 }
 pthread_mutex_unlock(&force_bounce_mutex);
 return result;
}
