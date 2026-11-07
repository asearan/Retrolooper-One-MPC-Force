// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include "ButtonProtocol.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "TrackBridge.h"
typedef struct _snd_rawmidi snd_rawmidi_t;
typedef struct _snd_rawmidi_info snd_rawmidi_info_t;
static int (*next_open)(snd_rawmidi_t**,snd_rawmidi_t**,const char*,int);
static ssize_t (*next_read)(snd_rawmidi_t*,void*,size_t);
static int (*next_close)(snd_rawmidi_t*);
static ssize_t (*next_write)(snd_rawmidi_t*,const void*,size_t);
static pthread_once_t init_once=PTHREAD_ONCE_INIT;
static snd_rawmidi_t* private_input;
static RloButtonParser parser;
static uint32_t rec_generation,ready;
/* Find libc dlsym without a third-party addin's interposed RTLD_NEXT origin. */
static void* next_symbol(const char* name){
    void*(*lookup)(void*,const char*)=0;
    const char* versions[]={"GLIBC_2.34","GLIBC_2.4","GLIBC_2.17","GLIBC_2.2.5","GLIBC_2.0"};
    for(size_t i=0;i<sizeof versions/sizeof*versions&&!lookup;++i)lookup=dlvsym(RTLD_DEFAULT,"dlsym",versions[i]);
    return (lookup?lookup:dlsym)(RTLD_NEXT,name);
}
static void init_symbols(void){next_open=next_symbol("snd_rawmidi_open");next_read=next_symbol("snd_rawmidi_read");next_close=next_symbol("snd_rawmidi_close");next_write=next_symbol("snd_rawmidi_write");}
static int controller_port(snd_rawmidi_t* h,int private_only){
    size_t(*size)(void)=next_symbol("snd_rawmidi_info_sizeof");
    int(*info)(snd_rawmidi_t*,snd_rawmidi_info_t*)=next_symbol("snd_rawmidi_info");
    const char*(*subname)(const snd_rawmidi_info_t*)=next_symbol("snd_rawmidi_info_get_subdevice_name");
    const char*(*devname)(const snd_rawmidi_info_t*)=next_symbol("snd_rawmidi_info_get_name");
    if(!size||!info||!subname||!devname)return 0;
    snd_rawmidi_info_t* ri=calloc(1,size());int found=0;
    if(ri&&info(h,ri)==0){const char*s=subname(ri),*d=devname(ri);
        found=s&&(!private_only||strstr(s,"Private"))&&((d&&(strstr(d,"MPC")||strstr(d,"Force")))||strstr(s,"MPC")||strstr(s,"Force"));}
    free(ri);return found;
}
#include "PadBridge.h"
__attribute__((visibility("default"))) uint32_t retrolooper_mpc_rec_generation(void){return __atomic_load_n(&rec_generation,__ATOMIC_ACQUIRE);}
__attribute__((visibility("default"))) uint32_t retrolooper_mpc_button_ready(void){return __atomic_load_n(&ready,__ATOMIC_ACQUIRE);}
__attribute__((visibility("default"))) int snd_rawmidi_open(snd_rawmidi_t**in,snd_rawmidi_t**out,const char*name,int mode){
    pthread_once(&init_once,init_symbols);if(!next_open)return -38;
    int rc=next_open(in,out,name,mode);
    if(rc>=0&&in&&*in&&controller_port(*in,1)){memset(&parser,0,sizeof parser);__atomic_store_n(&private_input,*in,__ATOMIC_RELEASE);__atomic_store_n(&ready,1,__ATOMIC_RELEASE);pad_default_bank();rlo_track_start();}
    if(rc>=0&&out&&*out&&controller_port(*out,0))pad_open_output(*out);
    if(rc>=0)pad_maybe_start();
    return rc;
}
__attribute__((visibility("default"))) ssize_t snd_rawmidi_read(snd_rawmidi_t*h,void*buf,size_t size){
    pthread_once(&init_once,init_symbols);if(!next_read)return -38;
    ssize_t n=next_read(h,buf,size);
    if(n>0&&h==__atomic_load_n(&private_input,__ATOMIC_ACQUIRE)){
        unsigned presses=rlo_button_bytes(&parser,buf,(size_t)n);
        if(presses)__atomic_fetch_add(&rec_generation,presses,__ATOMIC_RELEASE);
        pad_input_bytes(buf,(size_t)n);
    }
    return n; /* Native transport bytes are preserved; owned pad velocities may be suppressed. */
}
__attribute__((visibility("default"))) int snd_rawmidi_close(snd_rawmidi_t*h){
    pthread_once(&init_once,init_symbols);if(!next_close)return -38;
    if(h==__atomic_load_n(&private_input,__ATOMIC_ACQUIRE)){rlo_track_pause();__atomic_store_n(&private_input,0,__ATOMIC_RELEASE);__atomic_store_n(&ready,0,__ATOMIC_RELEASE);memset(&pad_parser,0,sizeof pad_parser);__atomic_store_n(&pad_bank,-1,__ATOMIC_RELEASE);}
    return pad_close_handle(h);
}
__attribute__((visibility("default"))) ssize_t snd_rawmidi_write(snd_rawmidi_t*h,const void*buffer,size_t size){
    pthread_once(&init_once,init_symbols);if(!next_write)return -38;return pad_native_write(h,buffer,size);
}
