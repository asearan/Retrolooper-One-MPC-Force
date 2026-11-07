// Local preview bridge. Never linked into the MPC release library.
#include "Vst2.h"
#include "Parameters.h"
#include <array>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
extern "C" AEffect* VSTPluginMain(HostCallback);
static uint32_t simulatedRecPresses=0;
extern "C" __attribute__((visibility("default"))) uint32_t retrolooper_mpc_rec_generation(){return simulatedRecPresses;}
extern "C" __attribute__((visibility("default"))) uint32_t retrolooper_mpc_button_ready(){return 1;}
static AEffect* previewPadOwner=nullptr;
static int previewPad=-1;
extern "C" __attribute__((visibility("default"))) uint32_t retrolooper_mpc_pad_publish(void* fx,uint32_t,uint32_t,const uint8_t*){return fx==previewPadOwner?15:1;}
extern "C" __attribute__((visibility("default"))) int retrolooper_mpc_pad_take(void* fx,uint32_t,uint32_t*pad){if(fx!=previewPadOwner||previewPad<0)return 0;*pad=previewPad;previewPad=-1;return 1;}
struct TestHost {AEffect* fx=nullptr;VstTimeInfo time{};double phase=0;uint64_t samples=0;std::vector<uint8_t> chunk;std::array<std::chrono::steady_clock::time_point,akai::param_count> lastPress{};};
static intptr_t previewHost(AEffect* e,int32_t op,int32_t,intptr_t,void*,float){
    auto* h=e?static_cast<TestHost*>(e->user):nullptr;
    return op==audioMasterGetTime&&h?reinterpret_cast<intptr_t>(&h->time):0;
}
#define API extern "C" __attribute__((visibility("default")))
API void* rlo_create(){
    auto* h=new TestHost;h->time.sampleRate=44100;h->time.tempo=120;h->time.timeSigNumerator=h->time.timeSigDenominator=4;
    h->time.cycleEndPos=16;h->time.flags=kVstTempoValid|kVstPpqPosValid|kVstTimeSigValid|kVstCyclePosValid|kVstTransportCycleActive;
    h->fx=VSTPluginMain(previewHost);
    if(!h->fx){delete h;return nullptr;}
    h->fx->user=h;previewPadOwner=h->fx;
    h->fx->dispatcher(h->fx,effSetSampleRate,0,0,nullptr,44100);
    if(!h->fx->dispatcher(h->fx,effMainsChanged,0,1,nullptr,0)){h->fx->dispatcher(h->fx,effClose,0,0,nullptr,0);delete h;return nullptr;}
    return h;
}
API void rlo_close(void* p){if(auto* h=static_cast<TestHost*>(p)){h->fx->dispatcher(h->fx,effClose,0,0,nullptr,0);delete h;}}
API void rlo_set(void* p,int param,float value){
    auto* h=static_cast<TestHost*>(p);
    // Offline renders run faster than real time. Distinct preview commands must
    // still represent distinct taps past the native host's touch debounce window.
    if(param>=0&&param<akai::param_count&&akai::parameters[param].kind==2&&value>=.5f){
        const auto ready=h->lastPress[param]+std::chrono::milliseconds(260);
        if(std::chrono::steady_clock::now()<ready)std::this_thread::sleep_until(ready);
        h->lastPress[param]=std::chrono::steady_clock::now();
    }
    h->fx->setParameter(h->fx,param,value);
}
API float rlo_get(void* p,int param){auto* h=static_cast<TestHost*>(p);return h->fx->getParameter(h->fx,param);}
API void rlo_display(void* p,int param,char* b){auto* h=static_cast<TestHost*>(p);h->fx->dispatcher(h->fx,effGetParamDisplay,param,0,b,0);}
API void rlo_render(void* p,int frames,float amplitude,double frequency,float* interleaved){
    auto* h=static_cast<TestHost*>(p);float il[128],ir[128],ol[128],orr[128];float* in[]{il,ir};float* out[]{ol,orr};
    for(int offset=0;offset<frames;offset+=128){const int n=std::min(128,frames-offset);for(int i=0;i<n;++i){const double envelope=.15+.85*std::exp(-6*std::fmod((h->samples++)/44100.,.5));il[i]=amplitude*envelope*std::sin(h->phase);ir[i]=il[i]*.5f;h->phase+=2*3.141592653589793*frequency/44100;}
        h->fx->processReplacing(h->fx,in,out,n);
        if(h->time.flags&kVstTransportPlaying){h->time.ppqPos+=n*h->time.tempo/(60*44100.);h->time.samplePos+=n;
            if(h->time.ppqPos>=h->time.cycleEndPos)h->time.ppqPos=std::fmod(h->time.ppqPos,h->time.cycleEndPos);}
        if(interleaved)for(int i=0;i<n;++i){interleaved[(offset+i)*2]=ol[i];interleaved[(offset+i)*2+1]=orr[i];}}
}
API int rlo_save(void* p){auto* h=static_cast<TestHost*>(p);void* data=nullptr;auto n=h->fx->dispatcher(h->fx,effGetChunk,0,0,&data,0);if(n<=0)return 0;const auto* ptr=static_cast<uint8_t*>(data);h->chunk.assign(ptr,ptr+n);return 1;}
API int rlo_reload(void* p){auto* h=static_cast<TestHost*>(p);if(h->chunk.empty())return 0;return h->fx->dispatcher(h->fx,effSetChunk,0,h->chunk.size(),h->chunk.data(),0)!=0;}

API void rlo_midi(void* p,int status,int number,int value){
    auto* h=static_cast<TestHost*>(p);VstMidiEvent m{};m.type=1;m.byteSize=sizeof m;m.midiData[0]=status;m.midiData[1]=number;m.midiData[2]=value;
    VstEvents ev{};ev.numEvents=1;ev.events[0]=reinterpret_cast<VstEvent*>(&m);h->fx->dispatcher(h->fx,effProcessEvents,0,0,&ev,0);
}

API void rlo_host_transport(void* p,int playing,int reset){auto* h=static_cast<TestHost*>(p);if(reset){h->time.ppqPos=0;h->time.samplePos=0;}if(!playing)h->time.flags&=~kVstTransportRecording;if(playing)h->time.flags|=kVstTransportPlaying;else h->time.flags&=~kVstTransportPlaying;}
API void rlo_host_sequence(void* p,int bars){auto* h=static_cast<TestHost*>(p);h->time.cycleEndPos=std::clamp(bars,1,64)*4.;h->time.ppqPos=std::fmod(h->time.ppqPos,h->time.cycleEndPos);}

API void rlo_host_recording(void* p,int recording){auto* h=static_cast<TestHost*>(p);if(recording)h->time.flags|=kVstTransportRecording;else h->time.flags&=~kVstTransportRecording;}

API void rlo_host_physical_rec(void* p){(void)p;++simulatedRecPresses;}

API void rlo_pad(void* p,int pad){auto* h=static_cast<TestHost*>(p);if(pad>=0&&pad<16){previewPadOwner=h->fx;previewPad=pad;}}
