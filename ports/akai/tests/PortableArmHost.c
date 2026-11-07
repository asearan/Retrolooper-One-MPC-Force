/* Minimal VST2 host named MPC with no helper symbols or native host ABI.
   Runs the actual release library in a separate process on each device. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/resource.h>
#include "portable-test-params.h"
#define CHECK(x) do{if(!(x)){dprintf(2,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
typedef struct Effect Effect;
typedef intptr_t (*Host)(Effect*,int32_t,int32_t,intptr_t,void*,float);
struct Effect {
    int32_t magic;
    intptr_t (*dispatch)(Effect*,int32_t,int32_t,intptr_t,void*,float);
    void (*process)(Effect*,float**,float**,int32_t);
    void (*set)(Effect*,int32_t,float);
    float (*get)(Effect*,int32_t);
    int32_t programs,params,inputs,outputs,flags;
    intptr_t reserved1,reserved2;
    int32_t delay,real,off;
    float ratio;
    void *object,*user;
    int32_t uid,version;
    void (*replace)(Effect*,float**,float**,int32_t);
};
typedef struct {
    double sample,rate,nanos,ppq,tempo,bar,cycle_start,cycle_end;
    int32_t numerator,denominator,smpte,smpte_rate,next_clock,flags;
} Time;
static Time t;
static intptr_t host(Effect* e,int32_t op,int32_t i,intptr_t value,void* ptr,float opt){
    (void)e;(void)i;(void)value;(void)ptr;(void)opt;return op==1?2400:op==7?(intptr_t)&t:0;
}
static double render(Effect* e,int frames,float level){
    float l[128],r[128],lo[128],ro[128];
    float* in[]={l,r};float* out[]={lo,ro};
    double energy=0;
    for(int i=0;i<frames;i+=128){
        int count=frames-i<128?frames-i:128;
        for(int n=0;n<count;++n){
            l[n]=level*sinf((float)(t.sample+n)*.04f);r[n]=l[n]*.5f;
        }
        e->replace(e,in,out,count);
        for(int n=0;n<count;++n){if(!isfinite(lo[n])||!isfinite(ro[n]))return -1;energy+=lo[n]*lo[n]+ro[n]*ro[n];}
        if(t.flags&2){t.sample+=count;t.ppq+=count*t.tempo/(60*t.rate);if((t.flags&4)&&t.ppq>=t.cycle_end)t.ppq-=t.cycle_end;}
    }
    return sqrt(energy/(2*frames));
}
static void tap(Effect*e,int p){e->set(e,p,1);e->set(e,p,0);}
static int is(Effect* e,int p,const char* expected){
    char str[24]={0};e->dispatch(e,7,p,0,str,0);
    if(strcmp(str,expected)){dprintf(2,"parameter %d: got '%s', expected '%s'\n",p,str,expected);return 0;}return 1;
}
static int run(Effect*(*entry)(Host),int rate,int cycle){
    t=(Time){.rate=rate,.tempo=120,.numerator=4,.denominator=4,.cycle_end=4,.flags=(1<<9)|(1<<10)|(1<<13)};
    if(cycle)t.flags|=(1<<12)|4;
    Effect* e=entry(host);CHECK(e&&e->uid==P_UID&&e->version==P_VERSION&&e->inputs==2&&e->outputs==2);
    e->dispatch(e,0,0,0,0,0);e->dispatch(e,10,0,0,0,(float)rate);
    e->set(e,P_BARS,0);e->set(e,P_CAPTURE_SOURCE,0);e->set(e,P_PAD_BANK_ENABLED,0);
#ifdef P_RECORD_OFFSET
    e->set(e,P_RECORD_OFFSET,1.f/51);
#endif
    CHECK(e->dispatch(e,12,0,1,0,0));render(e,128,0);
    usleep(150000);render(e,128,0);
    CHECK(is(e,P_CAPTURE_SOURCE,"Touch/Pads"));
    if(!cycle)CHECK(is(e,P_SEQUENCE_INFO,"TAKE 1 B"));
    t.flags|=2|8;render(e,512,.2f);CHECK(is(e,P_STATUS,"EMPTY"));
    t.flags&=~2;t.sample=t.ppq=0;render(e,128,0);
    tap(e,P_PAD_PRESS_01);render(e,128,0);CHECK(is(e,P_STATUS,"ARMED"));
    t.flags|=2;CHECK(render(e,rate*2,.25f)>.03);render(e,128,0);
    CHECK(is(e,P_STATUS,"PLAYING"));
    void* chunk=0;intptr_t n=e->dispatch(e,23,0,0,&chunk,0);
    uint32_t frames=0;CHECK(n>24&&chunk);memcpy(&frames,(char*)chunk+20,4);CHECK(frames==(uint32_t)rate*2);
    CHECK(render(e,4096,0)>.03);
    tap(e,P_DUB);render(e,128,.1f);CHECK(is(e,P_STATUS,"OVERDUB"));
    t.flags&=~(2|8);render(e,128,0);CHECK(is(e,P_STATUS,"STOPPED"));
    e->dispatch(e,1,0,0,0,0);
    dprintf(2,"PASS: release %d, %d Hz, %s, touch capture/overdub and host stop with no native Controls\n",P_VERSION,rate,cycle?"host cycle":"manual Take Bars");
    return 0;
}
#ifdef P_RECORD_OFFSET
static int run_offset(Effect*(*entry)(Host)){
    t=(Time){.rate=44100,.tempo=120,.numerator=4,.denominator=4,.cycle_end=4,.flags=(1<<9)|(1<<10)|(1<<12)|(1<<13)|4};
    Effect* e=entry(host);CHECK(e&&e->uid==P_UID&&e->version==P_VERSION);
    e->dispatch(e,0,0,0,0,0);e->dispatch(e,10,0,0,0,44100);
    CHECK(is(e,P_RECORD_OFFSET,"Auto (7 ms)"));
    CHECK(e->dispatch(e,12,0,1,0,0));render(e,128,0);usleep(150000);render(e,128,0);
    tap(e,P_PAD_PRESS_01);render(e,128,0);CHECK(is(e,P_STATUS,"ARMED"));
    t.flags|=2;
    const int delay=309,pulse=2048,period=88200;
    render(e,delay,0);render(e,pulse,.2f);render(e,period-2*pulse,0);render(e,pulse,.32f);render(e,16,0);
    CHECK(is(e,P_STATUS,"PLAYING"));
    void* chunk=0;const intptr_t n=e->dispatch(e,23,0,0,&chunk,0);
    uint32_t frames=0,params=0,targets=0;
    CHECK(n>44&&chunk);const char* c=chunk;
    memcpy(&frames,c+20,4);memcpy(&params,c+8,4);
    CHECK(frames==(unsigned)period&&params==(unsigned)e->params);
    memcpy(&targets,c+44+4*params,4);
    const size_t pcm=48+4*params+16*targets;
    CHECK((size_t)n==pcm+frames*8+4);
    double first=0,last=0;
    for(int i=0;i<pulse;++i){float l,r;memcpy(&l,c+pcm+4*i,4);memcpy(&r,c+pcm+4*(period-pulse+i),4);first+=l*l;last+=r*r;}
    CHECK(sqrt(first/pulse)>.13&&sqrt(first/pulse)<.15);
    CHECK(sqrt(last/pulse)>.21&&sqrt(last/pulse)<.24);
    e->dispatch(e,1,0,0,0,0);
    dprintf(2,"PASS: Force release %d corrects delayed analog input without changing the loop duration or dropping the tail\n",P_VERSION);
    return 0;
}
#endif
int main(int argc,char**argv){
    CHECK(argc==2);
    struct rlimit lim={650*1024*1024,650*1024*1024};
    CHECK(!setrlimit(RLIMIT_AS,&lim));alarm(90);
    void* lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!lib){dprintf(2,"%s\n",dlerror());return 1;}
    Effect*(*entry)(Host)=dlsym(lib,"VSTPluginMain");CHECK(entry);
    return run(entry,44100,1)||run(entry,48000,0)
#ifdef P_RECORD_OFFSET
        ||run_offset(entry)
#endif
        ;
}
