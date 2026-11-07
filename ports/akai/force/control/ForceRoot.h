#pragma once
/* Force 3.10: locate the owned application graph, rather than assuming the
   MPC One+'s global singleton address. Discovery is bounded and read-only. */
static uintptr_t native_application;
static const char *force_maps_path="/proc/self/maps";
static void discover_force_model(void){
 char map[32768],line[512],*cursor=map;int fd=open(force_maps_path,O_RDONLY|O_CLOEXEC);if(fd<0)return;
 ssize_t n=read(fd,map,sizeof map-1);close(fd);if(n<=0)return;map[n]=0;
 while(next_line(&cursor,line,sizeof line)){
  unsigned lo,hi;char perm[8];if(!strstr(line,"[heap]")||sscanf(line,"%x-%x %7s",&lo,&hi,perm)!=3||perm[0]!='r')continue;
  uint64_t end=hi;if(end>(uint64_t)lo+32*1024*1024)end=(uint64_t)lo+32*1024*1024;
  uint32_t block[16384];for(uint64_t at=lo;at<end;at+=sizeof block){size_t bytes=end-at<sizeof block?end-at:sizeof block;ssize_t got=pread(native_mem,block,bytes,at);if(got<=0)continue;
   for(unsigned i=0;i<(unsigned)got/4;++i){if(block[i]!=native_base+0x6cdeb14)continue;
    uintptr_t app=at+i*4;uint32_t window=native_word(app+0xc4),engine=native_word(window+0x26c);
    if(!word_is(window,native_base+0x6cdeeb4)||!word_is(engine,native_base+0x6ce4670))continue;
    uint32_t project=native_word(engine+0x264),mode=native_word(engine+0x118),queues=native_word(engine+0xfc);
    if(!word_is(project,native_base+0x6d4f1e4)||!word_is(mode,native_base+0x6ca2364)||!word_is(queues,native_base+0x6cb85ac)||!word_is(queues+0x18,native_base+0x6cb8534))continue;
    native_application=app;native_project=project;native_engine_mode=mode;native_queues=queues;return;
   }
  }
 }
}
