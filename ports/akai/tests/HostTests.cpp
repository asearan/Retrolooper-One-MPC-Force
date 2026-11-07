#include "Vst2.h"
#include "../control/TrackProtocol.h"
#include <memory>
#include <cstdlib>
#include "Parameters.h"
#include "CassetteTransport.h"
#include "WaveFile.h"
#include "LooperEngine.h"
#include "CaptureMemory.h"
#include "LoopFilters.h"
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
#ifdef RETROLOOPER_FORCE_BOUNCE
#include "../control/ClipProtocol.h"
#include <atomic>
static std::atomic<unsigned> clipRow{0};
static std::atomic<void*> pickerOwner{nullptr};
static std::atomic<unsigned> clipSubmissions{0};
static int clipCompletion=RLO_CLIP_BOUNCED;
static std::filesystem::path bouncedPath;
extern "C" __attribute__((visibility("default"))) int retrolooper_force_clip_target(void*,RloForceClipTarget* out) {
    const auto row=clipRow.load();
    *out={1,2,row,42,3,4};return row?RLO_CLIP_BOUNCED:RLO_CLIP_SELECTION_CHANGED;
}
extern "C" __attribute__((visibility("default"))) int retrolooper_force_clip_enqueue(void*,const RloForceClipTarget* target,const char* path,uint32_t* ticket) {
    if(target->row!=clipRow.load())return RLO_CLIP_SELECTION_CHANGED;
    pickerOwner.store(nullptr);bouncedPath=path;*ticket=++clipSubmissions;return RLO_CLIP_PENDING;
}
extern "C" __attribute__((visibility("default"))) int retrolooper_force_clip_result(uint32_t,int) {return clipCompletion;}
extern "C" __attribute__((visibility("default"))) int retrolooper_force_picker_begin(void* effect,uint32_t) {
    void* expected=nullptr;
    return pickerOwner.compare_exchange_strong(expected,effect)?RLO_CLIP_PENDING:RLO_CLIP_BUSY;
}
extern "C" __attribute__((visibility("default"))) int retrolooper_force_picker_poll(void* effect,uint32_t,RloForceClipTarget* out,int cancel) {
    if(pickerOwner.load()!=effect)return RLO_CLIP_SELECTION_CHANGED;
    if(cancel){pickerOwner.store(nullptr);return RLO_CLIP_SELECTION_CHANGED;}
    if(!clipRow.load())return RLO_CLIP_PENDING;
    return retrolooper_force_clip_target(effect,out);
}
#else
#include "../control/TimelineProtocol.h"
static bool timelineAvailable=false;
static int timelineResult=RLO_TIMELINE_BOUNCED;
static std::filesystem::path timelinePath;
extern "C" __attribute__((visibility("default"))) int retrolooper_mpc_timeline_target(void*,RloMpcTimelineTarget* out) {
    if(!timelineAvailable)return RLO_TIMELINE_UNAVAILABLE;
    *out={1,2,3,4};return RLO_TIMELINE_BOUNCED;
}
extern "C" __attribute__((visibility("default"))) int retrolooper_mpc_timeline_enqueue(void*,const RloMpcTimelineTarget* target,const char* path,uint32_t* ticket) {
    assert(target->project==1&&target->track==2&&target->sequence==3&&target->arrangement==4);
    timelinePath=path;*ticket=1;return RLO_TIMELINE_PENDING;
}
extern "C" __attribute__((visibility("default"))) int retrolooper_mpc_timeline_result(uint32_t ticket,int) {
    assert(ticket==1);return timelineResult;
}
#endif
extern "C" AEffect* VSTPluginMain(HostCallback);
extern "C" void rlo_test_conform_delay(int);
extern "C" uint64_t rlo_test_fallback_frames(AEffect*);
using namespace akai;
static uint32_t physicalRecordPresses=0;
static AEffect *bridgeSelected=nullptr,*bridgePrepareRequested=nullptr;
static AEffect *padReceiver=nullptr;static int queuedPad=-1;static std::array<uint8_t,48> lastPadRgb{};static uint32_t lastPadEnabled=0;
extern "C" __attribute__((visibility("default"))) uint32_t retrolooper_mpc_pad_publish(void* fx,uint32_t,uint32_t enabled,const uint8_t*rgb){
    if(fx==padReceiver){std::copy_n(rgb,48,lastPadRgb.begin());lastPadEnabled=enabled;}return 15;
}
extern "C" __attribute__((visibility("default"))) int retrolooper_mpc_pad_take(void* fx,uint32_t,uint32_t*pad){if(fx!=padReceiver||queuedPad<0)return 0;*pad=queuedPad;queuedPad=-1;return 1;}
static bool bridgeSupported=true,bridgeReady=true,bridgeDisarmed=true,bridgeInputReady=true;
extern "C" __attribute__((visibility("default"))) uint32_t retrolooper_mpc_track_status(void* fx){
    if(!bridgeSupported)return 0;
    uint32_t result=RLO_TRACK_SUPPORTED;if(!bridgeReady)return result;
    result|=RLO_TRACK_READY|RLO_TRACK_MONITOR_ROUTED;if(fx==bridgeSelected){result|=RLO_TRACK_SELECTED;if(bridgeInputReady)result|=RLO_TRACK_INPUT_READY;if(bridgeDisarmed)result|=RLO_TRACK_DISARMED;}
    return result;
}
extern "C" __attribute__((visibility("default"))) void retrolooper_mpc_track_prepare(void* fx){bridgePrepareRequested=static_cast<AEffect*>(fx);}

extern "C" __attribute__((visibility("default"))) uint32_t retrolooper_mpc_rec_generation(){return physicalRecordPresses;}
extern "C" __attribute__((visibility("default"))) uint32_t retrolooper_mpc_button_ready(){return 1;}
static VstTimeInfo hostTime{};
static bool supplyTime=false;
static std::array<int,param_count> reports{};
static int displayReports=0;
static intptr_t host(AEffect*,int32_t op,int32_t index,intptr_t,void*,float){if(op==audioMasterUpdateDisplay)++displayReports;if(op==audioMasterAutomate&&index>=0&&index<param_count)++reports[index];return op==audioMasterGetTime&&supplyTime?reinterpret_cast<intptr_t>(&hostTime):0;}
struct Host {
    AEffect* fx=VSTPluginMain(host);double phase=0,ppqJitter=0,lastPpqNoise=0;int jitterBlock=0;std::vector<float> lastLeft,lastRight;
    std::array<std::chrono::steady_clock::time_point,param_count> lastTap{};
    Host(bool freeRun=true,bool legacyCapture=true,[[maybe_unused]] bool autoOffset=false){
        assert(fx&&fx->magic==0x56737450);assert(fx->numInputs==2&&fx->numOutputs==2&&fx->numParams==param_count);
        fx->dispatcher(fx,effOpen,0,0,nullptr,0);
        if(freeRun)set(host_sync,0);
        fx->dispatcher(fx,effSetSampleRate,0,0,nullptr,44100);
        assert(fx->dispatcher(fx,effMainsChanged,0,1,nullptr,0));
        if(legacyCapture)set(capture_source,0);
#ifdef RETROLOOPER_FORCE_BOUNCE
        if(!autoOffset)set(record_offset,0); // Most fixtures inject audio on the timeline.
#endif
        set(monitor,0);set(crossfade_ms,0);
        std::array<float,128> z{};float* b[]{z.data(),z.data()};fx->processReplacing(fx,b,b,128);
    }
    ~Host(){fx->dispatcher(fx,effClose,0,0,nullptr,0);}
    void set(int p,float v){fx->setParameter(fx,p,(v-parameters[p].low)/(parameters[p].high-parameters[p].low));}
    void tap(int p){
        // Separate intentional GUI taps in wall time, as a human would; raw set()
        // remains available to reproduce duplicate native touch messages below.
        const auto ready=lastTap[p]+std::chrono::milliseconds(260);
        if(std::chrono::steady_clock::now()<ready)std::this_thread::sleep_until(ready);
        set(p,1);set(p,0);lastTap[p]=std::chrono::steady_clock::now();
    }
    std::string display(int p){char b[24]{};fx->dispatcher(fx,effGetParamDisplay,p,0,b,0);return b;}
    void render(int frames,float amplitude=0,double frequency=440,bool alias=false,bool legacy=false){
        lastLeft.resize(frames);lastRight.resize(frames);std::array<float,512> il{},ir{},ol{},orr{};
        for(int offset=0;offset<frames;offset+=128){const int n=std::min(128,frames-offset);for(int i=0;i<n;++i){il[i]=amplitude*std::sin(phase);ir[i]=amplitude*.5f*std::sin(phase);phase+=2*3.141592653589793*frequency/44100;ol[i]=orr[i]=legacy?.125f:0;}
            float* in[]{il.data(),ir.data()};float* out[]{alias?il.data():ol.data(),alias?ir.data():orr.data()};
            if(legacy)fx->process(fx,in,out,n);else fx->processReplacing(fx,in,out,n);
            for(int i=0;i<n;++i){assert(std::isfinite(out[0][i])&&std::isfinite(out[1][i]));lastLeft[offset+i]=out[0][i];lastRight[offset+i]=out[1][i];}
            if(supplyTime&&(hostTime.flags&kVstTransportPlaying)){
                hostTime.ppqPos+=n*hostTime.tempo/(60*44100.);hostTime.samplePos+=n;
                const double noise=ppqJitter*((++jitterBlock%2)?1:-1);
                hostTime.ppqPos+=noise-lastPpqNoise;lastPpqNoise=noise;
                if((hostTime.flags&kVstTransportCycleActive)&&(hostTime.flags&kVstCyclePosValid)&&hostTime.ppqPos>=hostTime.cycleEndPos)
                    hostTime.ppqPos=hostTime.cycleStartPos+std::fmod(hostTime.ppqPos-hostTime.cycleStartPos,hostTime.cycleEndPos-hostTime.cycleStartPos);
                if((hostTime.flags&kVstTransportCycleActive)&&(hostTime.flags&kVstCyclePosValid)&&hostTime.ppqPos<hostTime.cycleStartPos)
                    hostTime.ppqPos+=hostTime.cycleEndPos-hostTime.cycleStartPos;
            }
        }
    }
    double rms() const {double sum=0;for(float x:lastLeft)sum+=x*x;return std::sqrt(sum/std::max<std::size_t>(1,lastLeft.size()));}
    void recordTone(){tap(record);render(44100,.25);tap(record);render(128);assert(display(status)=="PLAYING");}
    void stopLoop(){tap(stop_now);render(512);assert(display(status)=="STOPPED");}
    std::vector<uint8_t> save(){void* ptr=nullptr;const auto n=fx->dispatcher(fx,effGetChunk,0,0,&ptr,0);assert(n>0&&ptr);return {static_cast<uint8_t*>(ptr),static_cast<uint8_t*>(ptr)+n};}
    bool load(const std::vector<uint8_t>& data){return fx->dispatcher(fx,effSetChunk,0,data.size(),const_cast<uint8_t*>(data.data()),0)!=0;}
    void midi(int status,int note,int velocity,int delta=0){VstMidiEvent m{};m.type=1;m.byteSize=sizeof m;m.deltaFrames=delta;m.midiData[0]=status;m.midiData[1]=note;m.midiData[2]=velocity;VstEvents ev{};ev.numEvents=1;ev.events[0]=reinterpret_cast<VstEvent*>(&m);fx->dispatcher(fx,effProcessEvents,0,0,&ev,0);}
    void waitJob(){for(int n=0;n<200;++n){std::this_thread::sleep_for(std::chrono::milliseconds(5));const auto s=display(message);if(s!="WORKING..."&&s!="READY TO RECORD")return;}assert(false&&"Worker timeout");}
};
int main(){
    std::cout<<"RetroLooper One native ABI and shared-engine tests\n";

    {
        supplyTime=true;hostTime={};hostTime.sampleRate=44100;hostTime.tempo=120;hostTime.timeSigNumerator=hostTime.timeSigDenominator=4;hostTime.cycleEndPos=4;
        hostTime.flags=kVstTempoValid|kVstPpqPosValid|kVstTimeSigValid|kVstCyclePosValid|kVstTransportCycleActive;
        Host touch(false,false);assert(touch.display(capture_source)=="Touch/Pads");touch.render(128);std::this_thread::sleep_for(std::chrono::milliseconds(150));touch.render(128);
        const float assignmentSteps=parameters[pad_a_16].high;const int original=std::lround(touch.fx->getParameter(touch.fx,pad_a_16)*assignmentSteps);
        touch.fx->setParameter(touch.fx,pad_a_16,(original+.51f)/assignmentSteps);assert(std::lround(touch.fx->getParameter(touch.fx,pad_a_16)*assignmentSteps)==original);
        touch.fx->setParameter(touch.fx,pad_a_16,(original+.8f)/assignmentSteps);assert(std::lround(touch.fx->getParameter(touch.fx,pad_a_16)*assignmentSteps)==original+1);
        touch.fx->setParameter(touch.fx,pad_a_16,(original+.5f)/assignmentSteps);assert(std::lround(touch.fx->getParameter(touch.fx,pad_a_16)*assignmentSteps)==original+1);
        touch.fx->setParameter(touch.fx,pad_a_16,(original+.2f)/assignmentSteps);assert(std::lround(touch.fx->getParameter(touch.fx,pad_a_16)*assignmentSteps)==original);
        ++physicalRecordPresses;touch.render(128);assert(touch.display(status)=="EMPTY");hostTime.flags|=kVstTransportPlaying|kVstTransportRecording;touch.render(4410,.2);assert(touch.display(status)=="EMPTY");
        hostTime.flags&=~(kVstTransportPlaying|kVstTransportRecording);touch.render(128);touch.tap(pad_press_01);touch.render(128);assert(touch.display(status)=="ARMED");
        const auto light=touch.fx->getParameter(touch.fx,pad_indicator_01);touch.render(11025);assert(touch.fx->getParameter(touch.fx,pad_indicator_01)!=light);
        hostTime.flags|=kVstTransportPlaying;hostTime.ppqPos=0;touch.render(88200,.2);touch.render(128);assert(touch.display(status)=="PLAYING");assert(touch.fx->getParameter(touch.fx,clear_indicator)==1&&touch.fx->getParameter(touch.fx,pad_indicator_07)==1);
        touch.set(dub_timing,2);touch.render(4410);touch.tap(pad_press_03);touch.render(128);assert(touch.display(status)=="DUB QUEUED");touch.tap(pad_press_03);touch.render(128);assert(touch.display(status)=="PLAYING");hostTime.ppqPos+=1;touch.render(128);assert(touch.display(status)=="PLAYING");
        hostTime.flags|=kVstTransportRecording;touch.render(128,.1);assert(touch.display(status)=="PLAYING");touch.set(dub_timing,1);touch.tap(pad_press_03);touch.render(128,.1);assert(touch.display(status)=="OVERDUB");
        hostTime.flags&=~kVstTransportRecording;touch.render(128,.1);assert(touch.display(status)=="OVERDUB");touch.tap(pad_press_03);touch.render(128);assert(touch.display(status)=="PLAYING");
        hostTime.flags&=~kVstTransportPlaying;touch.render(128);touch.tap(pad_press_03);touch.render(128);assert(touch.display(status)=="DUB QUEUED");const auto pending=touch.fx->getParameter(touch.fx,dub_indicator);touch.render(11025);assert(touch.fx->getParameter(touch.fx,dub_indicator)!=pending);
        hostTime.flags|=kVstTransportPlaying;hostTime.ppqPos=0;touch.render(128,.1);assert(touch.display(status)=="OVERDUB");touch.tap(pad_press_03);touch.render(128);assert(touch.display(status)=="PLAYING");
        touch.tap(pad_press_07);touch.render(128);assert(touch.display(status)=="EMPTY"&&touch.fx->getParameter(touch.fx,clear_indicator)==0&&touch.fx->getParameter(touch.fx,pad_indicator_07)==0);
        supplyTime=false;hostTime={};
        std::cout<<"PASS: touch/pad-only capture default, physical REC and MPC recording flags cannot capture, touch pad first take/overdub, cues blink, stopped overdub cue launches with MPC clock, loop-present purple lamp clears to dim\n";
    }
    {
        // An Arrange loop may start well into the timeline. Cue halfway in
        // and verify two known audio accents against the host over several
        // repeats and a stop/locate, with no native Controls available.
        bridgeSupported=false;supplyTime=true;hostTime={};hostTime.sampleRate=44100;hostTime.tempo=120;
        hostTime.timeSigNumerator=hostTime.timeSigDenominator=4;
        hostTime.cycleStartPos=12;hostTime.cycleEndPos=20;hostTime.ppqPos=16;
        hostTime.flags=kVstTempoValid|kVstPpqPosValid|kVstTimeSigValid|kVstCyclePosValid|kVstTransportCycleActive;
        Host arrange(false,false);arrange.tap(record);arrange.render(128);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        hostTime.flags|=kVstTransportPlaying;
        constexpr int quarter=22050,pulse=2048;
        arrange.render(4*quarter-256);assert(arrange.display(status)=="ARMED");
        arrange.render(256);
        for(int beat=0;beat<8;++beat){
            const float amp=beat==0?.2f:beat==5?.45f:0.f;
            arrange.render(pulse,amp);arrange.render(quarter-pulse);
        }
        for(int repeat=0;repeat<3;++repeat){
            for(int beat=0;beat<8;++beat){
                arrange.render(pulse);
                if(beat==0)assert(arrange.rms()>.12&&arrange.rms()<.16);
                else if(beat==5)assert(arrange.rms()>.29&&arrange.rms()<.34);
                else assert(arrange.rms()<.001);
                arrange.render(quarter-pulse);
            }
        }
        hostTime.flags&=~kVstTransportPlaying;arrange.render(128);
        hostTime.ppqPos=17;hostTime.flags|=kVstTransportPlaying;arrange.render(pulse);
        assert(arrange.rms()>.29&&arrange.rms()<.34);
        bridgeSupported=true;supplyTime=false;hostTime={};
        std::cout<<"PASS: standalone Arrange audio accents align to a nonzero timeline origin through repeats and locate\n";
    }
#ifdef RETROLOOPER_FORCE_BOUNCE
    {
        bridgeSupported=false;supplyTime=true;hostTime={};hostTime.sampleRate=44100;hostTime.tempo=120;
        hostTime.timeSigNumerator=hostTime.timeSigDenominator=4;hostTime.cycleStartPos=8;hostTime.cycleEndPos=12;hostTime.ppqPos=8;
        hostTime.flags=kVstTempoValid|kVstPpqPosValid|kVstTimeSigValid|kVstCyclePosValid|kVstTransportCycleActive;
        Host analog(false,false,true);
        assert(analog.display(record_offset)=="Auto (7 ms)");
        analog.tap(record);analog.render(128);std::this_thread::sleep_for(std::chrono::milliseconds(100));
        hostTime.flags|=kVstTransportPlaying;
        constexpr int delay=309,pulse=2048,period=88200,beat=22050;
        analog.render(delay);analog.render(pulse,.2f);analog.render(period-2*pulse);analog.render(pulse,.32f);analog.render(16);
        assert(analog.display(status)=="PLAYING");
        auto readTake=[&](int first,int count){
            const auto bytes=analog.save();
            assert(u32(bytes.data()+20)==period);
            const size_t pcm=44+4*param_count+4+16*midiTargetCount;
            double sum=0;for(int i=first;i<first+count;++i){
                float sample;std::memcpy(&sample,bytes.data()+pcm+4*i,4);sum+=sample*sample;
            }
            return std::sqrt(sum/count);
        };
        assert(readTake(0,pulse)>.13&&readTake(0,pulse)<.15);
        assert(readTake(period-pulse,pulse)>.21&&readTake(period-pulse,pulse)<.24);
        assert(readTake(beat,pulse)<.001);
        analog.set(dub_timing,1);analog.tap(dub);analog.render(beat-16);
        analog.render(pulse,.12f);analog.tap(dub);analog.render(16);
        assert(analog.display(status)=="PLAYING");
        assert(readTake(beat,pulse)>.079&&readTake(beat,pulse)<.09);
        assert(readTake(beat+pulse,delay)<.001);
        const auto project=analog.save();Host recalled(false,false);assert(recalled.load(project));
        assert(recalled.display(record_offset)=="Auto (7 ms)");
        for(const float oldOffset:{0.f,8.f}){
            auto old=project;
            const uint32_t oldVersion=2;
            const float normalized=oldOffset/50;
            std::memcpy(old.data()+4,&oldVersion,4);
            std::memcpy(old.data()+44+record_offset*4,&normalized,4);
            uint32_t checksum=2166136261u;
            for(size_t i=0;i+4<old.size();++i)checksum=(checksum^old[i])*16777619u;
            std::memcpy(old.data()+old.size()-4,&checksum,4);
            assert(recalled.load(old));
            assert(recalled.display(record_offset)==(oldOffset==0?"Auto (7 ms)":"8 ms"));
        }
        bridgeSupported=true;supplyTime=false;hostTime={};
        std::cout<<"PASS: Force analog offset aligns first and last accents and overdubs, keeps exact loop length and recalls offset\n";
    }
#endif
    {
        setenv("RETROLOOPER_TEST_TRACK_GUARD","1",1);
        supplyTime=true;hostTime={};hostTime.sampleRate=44100;hostTime.tempo=120;
        hostTime.timeSigNumerator=hostTime.timeSigDenominator=4;hostTime.cycleEndPos=4;
        hostTime.flags=kVstTempoValid|kVstPpqPosValid|kVstTimeSigValid|kVstCyclePosValid|kVstTransportCycleActive;
        std::array<std::unique_ptr<Host>,4> tracks;
        for(auto& h:tracks)h=std::make_unique<Host>(false);
        unsetenv("RETROLOOPER_TEST_TRACK_GUARD");
        auto renderAll=[&](int frames,int inputTrack=-1,float amplitude=0.f){
            for(int offset=0;offset<frames;offset+=128){
                const int n=std::min(128,frames-offset);const auto sameTime=hostTime;
                for(int i=0;i<4;++i){hostTime=sameTime;tracks[i]->render(n,i==inputTrack||inputTrack==-2?amplitude:0.f,220+i*110);}
            }
        };
        bridgeSelected=tracks[0]->fx;bridgeReady=true;bridgeDisarmed=true;
        renderAll(128);std::this_thread::sleep_for(std::chrono::milliseconds(150));renderAll(128);
        renderAll(512,-2,.2f);
        // Incoming insert buffers may contain native timeline clips. Preserve
        // those even on unselected tracks; gate live input in MPC's monitor path.
        for(int i=0;i<4;++i)assert(tracks[i]->rms()>.04);
        bridgeSelected=tracks[1]->fx;renderAll(512,-2,.2f);
        for(int i=0;i<4;++i)assert(tracks[i]->rms()>.04);
        tracks[2]->fx->dispatcher(tracks[2]->fx,effMainsChanged,0,0,nullptr,0);renderAll(128,-2,.2f);assert(tracks[2]->rms()>.04);
        tracks[2]->fx->dispatcher(tracks[2]->fx,effMainsChanged,0,1,nullptr,0);
        bridgeSelected=tracks[0]->fx;renderAll(128);
        tracks[1]->tap(record);renderAll(128);
        assert(tracks[1]->display(message)=="SELECT THIS AUDIO TRACK");
        bridgeSelected=tracks[1]->fx;renderAll(128);
        assert(tracks[1]->display(message)=="READY TO RECORD"&&tracks[1]->display(status)=="EMPTY");
        bridgeSelected=tracks[0]->fx;renderAll(128);
        ++physicalRecordPresses;renderAll(128);
        assert(tracks[0]->display(status)=="ARMED");
        // A slow MPC UI refresh temporarily withholds ownership acknowledgement.
        // Block capture, but retain an explicit first-take cue until the host
        // confirms a different selected track or the user cancels it.
        bridgeReady=false;renderAll(128);
        assert(tracks[0]->display(status)=="ARMED"&&"A temporary bridge refresh must not silently disarm the first take");
        bridgeReady=true;bridgeInputReady=false;renderAll(128);
        assert(tracks[0]->display(status)=="ARMED");
        bridgeInputReady=true;renderAll(128);
        for(int i=1;i<4;++i)assert(tracks[i]->display(status)=="EMPTY");
        hostTime.ppqPos=hostTime.samplePos=0;
        hostTime.flags|=kVstTransportPlaying|kVstTransportRecording;
        renderAll(88200,0,.2f);renderAll(128);
        assert(tracks[0]->display(status)=="OVERDUB");
        for(int i=1;i<4;++i)assert(tracks[i]->display(status)=="EMPTY");
        tracks[0]->set(dub_timing,1);tracks[0]->tap(dub);renderAll(128);
        assert(tracks[0]->display(status)=="PLAYING"&&(hostTime.flags&kVstTransportRecording));
        hostTime.ppqPos+=1;renderAll(256);
        assert(tracks[0]->display(status)=="PLAYING"&&"A touch End Dub must survive host position rebases without changing MPC transport");
        tracks[0]->tap(stop_now);renderAll(128);assert(tracks[0]->display(status)=="STOPPED");
        hostTime.ppqPos+=.5;renderAll(256);assert(tracks[0]->display(status)=="STOPPED"&&(hostTime.flags&kVstTransportPlaying));
        tracks[0]->tap(play);renderAll(128);assert(tracks[0]->display(status)=="PLAYING");
        tracks[0]->tap(record);renderAll(128);assert(tracks[0]->display(status)=="OVERDUB"&&tracks[0]->display(duration)=="2.00 SEC");
        tracks[0]->tap(record);renderAll(128);assert(tracks[0]->display(status)=="PLAYING");
        tracks[0]->tap(dub);renderAll(128);assert(tracks[0]->display(status)=="OVERDUB");
        hostTime.flags&=~kVstTransportRecording;renderAll(128);assert(tracks[0]->display(status)=="PLAYING");
        tracks[0]->tap(dub);renderAll(128);hostTime.ppqPos+=.5;renderAll(128);
        assert(tracks[0]->display(status)=="OVERDUB"&&!(hostTime.flags&kVstTransportRecording));
        hostTime.flags|=kVstTransportRecording;renderAll(128);
        bridgeSelected=tracks[1]->fx;renderAll(128);
        assert(tracks[0]->display(status)=="PLAYING"&&tracks[0]->rms()>.04);
        assert(tracks[1]->display(status)=="EMPTY");
        ++physicalRecordPresses;renderAll(176400,1,.15f);renderAll(128);
        assert(tracks[1]->display(status)=="OVERDUB");
        assert(tracks[0]->display(status)=="PLAYING"&&tracks[0]->rms()>.04);
        assert(tracks[2]->display(status)=="EMPTY"&&tracks[3]->display(status)=="EMPTY");
        bridgeSelected=tracks[0]->fx;renderAll(128);
        assert(tracks[0]->display(status)=="OVERDUB"&&tracks[1]->display(status)=="PLAYING");
        bridgeSelected=tracks[2]->fx;renderAll(128);
        assert(tracks[0]->display(status)=="PLAYING"&&tracks[0]->rms()>.04);
        hostTime.flags&=~kVstTransportRecording;
        bridgeSelected=tracks[0]->fx;renderAll(128);
        tracks[0]->set(capture_source,1);tracks[0]->set(dub_timing,2);
        hostTime.ppqPos=1;tracks[0]->tap(dub);renderAll(128);
        assert(tracks[0]->display(status)!="OVERDUB");
        bridgeSelected=tracks[1]->fx;renderAll(176400);
        assert(tracks[0]->display(status)=="PLAYING");
        // Native arm acknowledgement must precede even an explicit local cue.
        bridgeSelected=tracks[3]->fx;bridgeDisarmed=false;bridgeInputReady=false;bridgePrepareRequested=nullptr;
        tracks[3]->tap(record);renderAll(128);
        assert(bridgePrepareRequested==tracks[3]->fx);
        assert(tracks[3]->display(message)=="PREPARING MPC TRACK");
        assert(tracks[3]->display(status)=="EMPTY");
        bridgeDisarmed=true;renderAll(128);assert(tracks[3]->display(status)=="EMPTY");
        bridgeInputReady=true;renderAll(128);
        assert(tracks[3]->display(status)=="ARMED");
        bridgeSelected=tracks[2]->fx;renderAll(176400);
        assert(tracks[3]->display(status)=="EMPTY");
        // Automatic host-record handoff also prepares an armed native track.
        bridgeSelected=tracks[0]->fx;tracks[0]->set(capture_source,0);
        bridgeDisarmed=false;bridgePrepareRequested=nullptr;
        hostTime.flags|=kVstTransportRecording;renderAll(128);
        assert(bridgePrepareRequested==tracks[0]->fx&&tracks[0]->display(status)=="PLAYING");
        bridgeDisarmed=true;renderAll(128);
        assert(tracks[0]->display(status)=="OVERDUB");
        // The real MPC can rebase PPQ after starting playback. It must not
        // leave the selected looper in PLAYING while global REC remains set.
        hostTime.ppqPos+=1.;renderAll(128);
        assert(tracks[0]->display(status)=="OVERDUB"&&tracks[1]->display(status)=="PLAYING");
        renderAll(176400,0,.05f);
        assert(tracks[0]->display(status)=="OVERDUB"&&tracks[1]->display(status)=="PLAYING");
        // Losing the bridge closes capture but preserves audible loop playback.
        bridgeReady=false;renderAll(128);
        assert(tracks[0]->display(status)=="PLAYING"&&tracks[0]->rms()>.04);
        bridgeReady=true;bridgeSelected=nullptr;bridgePrepareRequested=nullptr;
        supplyTime=false;hostTime={};
        std::cout<<"PASS: four-instance selected-track capture, physical REC isolation, continuous unselected playback, live overdub handoff, canceled future capture, native arm acknowledgement and bridge-loss protection\n";
    }

    {
        supplyTime=true;hostTime={};hostTime.sampleRate=44100;hostTime.tempo=120;hostTime.timeSigNumerator=hostTime.timeSigDenominator=4;hostTime.cycleEndPos=8;
        hostTime.flags=kVstTempoValid|kVstPpqPosValid|kVstTimeSigValid|kVstCyclePosValid|kVstTransportCycleActive;
        Host pads(false);padReceiver=pads.fx;pads.render(128);std::this_thread::sleep_for(std::chrono::milliseconds(150));pads.render(4410);
        auto press=[&](int pad){queuedPad=pad-1;pads.render(128);};
        press(1);assert(pads.display(status)=="ARMED"&&!(hostTime.flags&kVstTransportPlaying));
        hostTime.flags|=kVstTransportPlaying;hostTime.ppqPos=0;pads.render(176400,.2f);pads.render(128);assert(pads.display(status)=="PLAYING"&&pads.display(duration)=="4.00 SEC");
        press(2);pads.render(88200);assert(pads.display(status)=="STOPPED"&&(hostTime.flags&kVstTransportPlaying));
        press(2);assert(pads.display(status)=="PLAYING");
        press(8);pads.waitJob();assert(pads.display(message)=="EDIT COMPLETE");
        int muteVisible=0;for(int i=0;i<midiVisibleCount;++i)if(midiTargets[midiVisibleTargets[i]]==mute)muteVisible=i;
        pads.set(learn_target,muteVisible);pads.set(learn,1);press(16);
        assert(pads.display(pad_a_16)=="Mute"&&pads.display(learn)=="Off"&&pads.display(pad_label_mute)=="A4+");
        pads.set(mute_timing,1);press(16);pads.render(4410);assert(pads.display(status)=="MUTED"&&pads.fx->getParameter(pads.fx,mute_indicator)==1);
        assert(std::equal(lastPadRgb.begin()+3*3,lastPadRgb.begin()+3*4,lastPadRgb.begin()+15*3));
        assert(lastPadEnabled&1);pads.set(pad_a_16,0);press(16);assert(pads.display(status)=="MUTED");
        pads.set(pad_bank_enabled,0);press(4);pads.render(4410);assert(pads.display(status)=="MUTED"&&lastPadEnabled==0);
        const auto mapped=pads.save();{Host restored(false);assert(restored.load(mapped));assert(restored.display(pad_a_16)=="Off"&&restored.display(pad_bank_enabled)=="Off");}
        padReceiver=nullptr;queuedPad=-1;supplyTime=false;hostTime={};
        std::cout<<"PASS: physical-pad action path, MPC-only global transport, default Record/Play/Reverse, pad reassignment through Learn, matching RGB, disabled/off pad safety and project recall\n";
    }
    {
        supplyTime=true;hostTime={};hostTime.sampleRate=44100;hostTime.tempo=120;
        hostTime.timeSigNumerator=hostTime.timeSigDenominator=4;hostTime.cycleEndPos=4;
        hostTime.flags=kVstTempoValid|kVstPpqPosValid|kVstTimeSigValid|kVstCyclePosValid|kVstTransportCycleActive;
        Host threshold(false);threshold.set(auto_start,1);threshold.set(start_threshold,-12);
        threshold.render(128);std::this_thread::sleep_for(std::chrono::milliseconds(150));threshold.render(128);
        hostTime.flags|=kVstTransportPlaying|kVstTransportRecording;
        threshold.render(512,.8);assert(threshold.display(status)=="EMPTY"); // Still needs REC.
        hostTime.flags&=~(kVstTransportPlaying|kVstTransportRecording);threshold.render(128);
        ++physicalRecordPresses;threshold.render(128,.8);
        assert(threshold.display(status)=="ARMED"&&threshold.display(message)=="WAIT MPC PLAY");
        bool lit=false,dark=false;
        for(int i=0;i<4;++i){threshold.render(11025);const bool on=threshold.fx->getParameter(threshold.fx,record_indicator)>.5f;lit|=on;dark|=!on;}
        assert(lit&&dark&&"An armed first take must blink while waiting for MPC Play");
        hostTime.ppqPos=.5;hostTime.samplePos=11025;hostTime.flags|=kVstTransportPlaying|kVstTransportRecording;
        threshold.render(128,.01);assert(threshold.display(status)=="ARMED");
        std::array<float,128> left{},right{},outL{},outR{};
        float* in[]{left.data(),right.data()};float* out[]{outL.data(),outR.data()};
        threshold.set(input,1);right.fill(.8f);left[4]=std::nanf("");left[8]=std::numeric_limits<float>::infinity();
        threshold.fx->processReplacing(threshold.fx,in,out,128);hostTime.ppqPos+=128./22050;hostTime.samplePos+=128;
        assert(threshold.display(status)=="ARMED"); // Unselected input/nonfinite samples cannot trigger.
        threshold.set(input,0);threshold.set(input_gain,0);left.fill(.8f);
        threshold.fx->processReplacing(threshold.fx,in,out,128);hostTime.ppqPos+=128./22050;hostTime.samplePos+=128;
        assert(threshold.display(status)=="ARMED");
        threshold.set(input_gain,1);left.fill(0);right.fill(0);left[37]=.4f;right[37]=.8f;
        const int phase=static_cast<int>(std::llround(hostTime.ppqPos*22050))+37;
        threshold.fx->processReplacing(threshold.fx,in,out,128);
        hostTime.ppqPos+=128./22050;hostTime.samplePos+=128;
        assert(threshold.display(status)=="RECORDING");
        assert(threshold.fx->getParameter(threshold.fx,record_indicator)==1);
        threshold.render(88200-(128-37));assert(threshold.display(status)=="OVERDUB");
        assert(threshold.fx->getParameter(threshold.fx,record_indicator)==0&&threshold.fx->getParameter(threshold.fx,dub_indicator)==1&&threshold.fx->getParameter(threshold.fx,play_indicator)==1);
        auto take=threshold.save();assert(u32(take.data()+20)==88200);
        const auto audioAt=44+param_count*4+4+4*midiTargetCount*4;
        auto sample=[&](const std::vector<uint8_t>& chunk,int channel,int index){float v;std::memcpy(&v,chunk.data()+audioAt+(channel*88200+index)*4,4);return v;};
        if(std::abs(sample(take,0,phase)-.4f)>=1e-6||std::abs(sample(take,1,phase)-.8f)>=1e-6){
            std::cerr<<"Threshold expected phase "<<phase<<" actual L/R "<<sample(take,0,phase)<<" / "<<sample(take,1,phase)<<"\n";
            for(int i=0,shown=0;i<88200&&shown<8;++i)if(std::abs(sample(take,1,i))>.001){std::cerr<<"nonzero "<<i<<" = "<<sample(take,1,i)<<"\n";++shown;}
        }
        assert(std::abs(sample(take,0,phase)-.4f)<1e-6&&std::abs(sample(take,1,phase)-.8f)<1e-6);
        assert(sample(take,0,0)==0&&sample(take,1,phase-1)==0&&sample(take,1,phase+1)==0);
        hostTime.flags&=~(kVstTransportPlaying|kVstTransportRecording);threshold.render(128);
        assert(threshold.display(status)=="STOPPED");
        hostTime.ppqPos=hostTime.samplePos=0;hostTime.flags|=kVstTransportPlaying;threshold.render(88200);
        assert(std::abs(threshold.lastLeft[phase]-.4f)<.002); // Restart keeps MPC phase.
        hostTime.flags&=~kVstTransportPlaying;threshold.render(128);threshold.tap(clear);threshold.render(128);
        ++physicalRecordPresses;threshold.render(128);
        hostTime.ppqPos=1;hostTime.samplePos=22050;hostTime.flags|=kVstTransportPlaying;
        left.fill(0);right.fill(0);right[37]=.8f;
        threshold.fx->processReplacing(threshold.fx,in,out,128);hostTime.ppqPos+=128./22050;hostTime.samplePos+=128;
        assert(threshold.display(status)=="RECORDING");
        hostTime.flags&=~kVstTransportPlaying;threshold.render(128);
        auto partial=threshold.save();assert(u32(partial.data()+20)==88200);
        assert(std::abs(sample(partial,1,22050+37)-.8f)<1e-6&&sample(partial,1,22050+36)==0&&sample(partial,1,0)==0);
        Host recalled(false);assert(recalled.load(partial));assert(recalled.display(status)=="STOPPED");
        assert(recalled.display(auto_start)=="On"&&recalled.display(start_threshold)=="-12.0 dB");
        // v0.3.2: 181 parameters, 56 stored MIDI targets, 49 visible targets.
        threshold.set(learn_target,48);auto current=threshold.save();
        std::vector<uint8_t> old(current.begin(),current.begin()+44+181*4);
        const uint32_t oldCount=181,oldTargets=56;std::memcpy(old.data()+8,&oldCount,4);float oldSelected=1;std::memcpy(old.data()+44+learn_target*4,&oldSelected,4);
        const auto* targetBytes=reinterpret_cast<const uint8_t*>(&oldTargets);old.insert(old.end(),targetBytes,targetBytes+4);
        for(int group=0;group<4;++group){const auto at=44+param_count*4+4+group*midiTargetCount*4;old.insert(old.end(),current.begin()+at,current.begin()+at+56*4);}
        old.insert(old.end(),current.begin()+audioAt,current.end()-4);uint32_t checksum=2166136261u;for(auto b:old)checksum=(checksum^b)*16777619u;
        const auto* checkBytes=reinterpret_cast<const uint8_t*>(&checksum);old.insert(old.end(),checkBytes,checkBytes+4);
        assert(recalled.load(old));assert(recalled.display(auto_start)=="Off"&&recalled.display(start_threshold)=="-36.0 dB");
        assert(recalled.display(learn_target)=="Loop Launch"&&recalled.display(binding)=="NOTE -1 / CC -1");
        supplyTime=false;hostTime={};
        std::cout<<"PASS: explicit REC threshold arming, stopped/below-threshold protection, sample-offset immediate capture, sequence-phase playback/restart, partial-take silence, recall and v0.3.2 mapping migration\n";
    }

    {
        auto normalizedHz=[](double hz){return static_cast<float>(std::log(hz/20)/std::log(1000.));};
        for(double sr:{44100.,48000.}){
            LoopFilters identity;identity.prepare(sr);
            for(int i=0;i<2048;++i){float l=std::sin(i*.13f),r=l*.3f;const float a=l,b=r;identity.process(l,r);assert(l==a&&r==b);}
            auto gain=[&](bool high,double frequency){
                LoopFilters f;f.prepare(sr);f.set(high?normalizedHz(1000):0,high?1:normalizedHz(1000));
                double sum=0;for(int i=0;i<int(sr);++i){float l=.25f*std::sin(6.283185307179586*frequency*i/sr),r=0;f.process(l,r);assert(std::isfinite(l)&&r==0);if(i>sr/2)sum+=l*l;}
                return std::sqrt(sum/(sr/2));
            };
            assert(gain(true,100)<gain(true,5000)*.03);
            assert(gain(false,10000)<gain(false,100)*.02);
            LoopFilters sweep;sweep.prepare(sr);
            for(int i=0;i<20000;++i){if(i%128==0)sweep.set((i/128)%2,((i/128)%3)?0:1);float l=.3f,r=-.2f;sweep.process(l,r);assert(std::isfinite(l)&&std::isfinite(r)&&std::abs(l)<2&&std::abs(r)<2);}
        }
        std::cout<<"PASS: HP/LP identity bypass, 12 dB/octave rejection, stereo isolation and smoothed extreme sweeps at 44.1/48 kHz\n";
    }
    {
        // A constant test tone cannot expose a two-bar take repeating only its
        // first bar. Four distinct sections also expose blockwise PPQ resampling.
        supplyTime=true;hostTime={};hostTime.tempo=120;hostTime.sampleRate=44100;
        hostTime.timeSigNumerator=hostTime.timeSigDenominator=4;hostTime.cycleEndPos=8;
        hostTime.flags=kVstTempoValid|kVstPpqPosValid|kVstTimeSigValid|kVstCyclePosValid|kVstTransportCycleActive;
        Host multiBar(false);multiBar.render(128);std::this_thread::sleep_for(std::chrono::milliseconds(100));
        multiBar.tap(record);multiBar.render(128);hostTime.flags|=kVstTransportPlaying;
        for(int section=0;section<4;++section)multiBar.render(44100,.1f*(section+1),217+section*214);
        multiBar.render(128);assert(multiBar.display(status)=="PLAYING");
        assert(u32(multiBar.save().data()+20)==176400);
        for(int section=0;section<4;++section){
            multiBar.render(44100);
            const double expected=.1*(section+1)/std::sqrt(2.);
            if(std::abs(multiBar.rms()-expected)>.005)std::cerr<<"Two-bar section "<<section<<" RMS "<<multiBar.rms()<<" expected "<<expected<<"\n";
            assert(std::abs(multiBar.rms()-expected)<.005);
        }
        multiBar.ppqJitter=.001;
        multiBar.render(44100);const auto noisyHostAudio=multiBar.lastLeft;
        multiBar.render(44100*3);multiBar.ppqJitter=0;multiBar.lastPpqNoise=0;
        hostTime.ppqPos=std::fmod(hostTime.ppqPos,8.);
        multiBar.render(44100);const auto stableHostAudio=multiBar.lastLeft;
        double difference=0;for(std::size_t i=0;i<noisyHostAudio.size();++i)difference=std::max(difference,std::abs(double(noisyHostAudio[i]-stableHostAudio[i])));
        assert(difference<1.e-5&&"MPC PPQ jitter must not modulate recorded audio");
        const auto exportPath=std::filesystem::path(std::getenv("RETROLOOPER_TEST_LOOPS"))/"Loop-58.wav";
        std::filesystem::remove(exportPath);multiBar.set(file_slot,58);multiBar.tap(export_audio);multiBar.waitJob();
        assert(multiBar.display(message)=="WAV EXPORTED"&&multiBar.display(status)=="PLAYING");
        const auto exported=readWave(exportPath);assert(exported.left.size()==176400&&exported.right.size()==176400);
        {std::ifstream f(exportPath,std::ios::binary);std::array<unsigned char,44> header{};f.read(reinterpret_cast<char*>(header.data()),header.size());assert(u16(header.data()+20)==1&&u16(header.data()+34)==24);}
        // Default launch adopts sequence phase; Next bar waits then reads loop 0.
        const auto sequence=multiBar.save();
        hostTime.flags&=~kVstTransportPlaying;hostTime.ppqPos=1;
        Host phase(false);assert(phase.load(sequence));phase.render(128);
        hostTime.flags|=kVstTransportPlaying;phase.render(128);
        assert(phase.fx->getParameter(phase.fx,position)>.12&&phase.fx->getParameter(phase.fx,position)<.13);
        hostTime.flags&=~kVstTransportPlaying;phase.render(128);phase.set(launch_mode,1);hostTime.ppqPos=1;
        hostTime.flags|=kVstTransportPlaying;bool playLit=false,playDark=false;
        for(int i=0;i<5;++i){phase.render(11025);const bool lit=phase.fx->getParameter(phase.fx,play_indicator)>.5f;playLit|=lit;playDark|=!lit;assert(phase.fx->getParameter(phase.fx,record_indicator)==0);}
        assert(playLit&&playDark);
        phase.render(11025-128);
        assert(phase.display(status)=="ARMED"&&phase.rms()==0);
        phase.render(256);assert(phase.display(status)=="PLAYING");
        assert(phase.fx->getParameter(phase.fx,position)<.01);
        phase.render(44100);assert(phase.rms()>.06&&phase.rms()<.08);
        phase.render(44100*2);assert(phase.fx->getParameter(phase.fx,position)>.74);
        hostTime.flags&=~kVstTransportPlaying;phase.render(128);phase.set(launch_mode,0);hostTime.ppqPos=0;
        hostTime.flags|=kVstTransportPlaying;phase.render(11025);phase.set(dub_timing,2);phase.tap(dub);
        bool dubLit=false,dubDark=false;
        for(int i=0;i<6;++i){phase.render(11025);const bool lit=phase.fx->getParameter(phase.fx,dub_indicator)>.5f;dubLit|=lit;dubDark|=!lit;assert(phase.display(status)=="DUB QUEUED");}
        assert(dubLit&&dubDark);phase.render(22050);assert(phase.display(status)=="OVERDUB"&&phase.fx->getParameter(phase.fx,dub_indicator)==1);
        phase.set(mute_timing,2);phase.tap(mute);bool muteLit=false,muteDark=false;
        for(int i=0;i<6;++i){phase.render(11025);const bool lit=phase.fx->getParameter(phase.fx,mute_indicator)>.5f;muteLit|=lit;muteDark|=!lit;assert(phase.fx->getParameter(phase.fx,dub_indicator)==1);}
        assert(muteLit&&muteDark);phase.render(22050);assert(phase.display(status)=="MUTED"&&phase.fx->getParameter(phase.fx,mute_indicator)==1);
        phase.render(11025);assert(phase.fx->getParameter(phase.fx,mute_indicator)==1&&!(hostTime.flags&kVstTransportRecording));
        auto waitLength=[](Host& h,const std::string& expected){for(int n=0;n<200;++n){h.render(128);if(h.display(duration)==expected)return;std::this_thread::sleep_for(std::chrono::milliseconds(5));}assert(false&&"Automatic sequence-length update timed out");};
        hostTime.flags&=~kVstTransportPlaying;hostTime.ppqPos=0;hostTime.cycleEndPos=4;
        Host window(false);assert(window.load(sequence));waitLength(window,"2.00 SEC");
        const auto shortened=window.save();assert(u32(shortened.data()+20)==176400); // full source remains in the project
        Host expanded(false);assert(expanded.load(shortened));assert(expanded.display(duration)=="2.00 SEC");
        hostTime.cycleEndPos=8;waitLength(expanded,"4.00 SEC");hostTime.flags|=kVstTransportPlaying;hostTime.ppqPos=0;
        expanded.render(44100*2);expanded.render(44100);assert(expanded.rms()>.20&&expanded.rms()<.22);
        hostTime.flags&=~kVstTransportPlaying;hostTime.ppqPos=0;
        Audio constant;constant.left.resize(176400);constant.right.resize(176400);
        for(int i=0;i<176400;++i){constant.left[i]=.125f*std::sin(6.283185307179586*217*i/44100);constant.right[i]=constant.left[i]*-.5f;}
        const auto constantPath=std::filesystem::path(std::getenv("RETROLOOPER_TEST_LOOPS"))/"Loop-57.wav";std::filesystem::remove(constantPath);writeWave(constantPath,constant);
        Host growing(false);growing.set(file_slot,57);growing.tap(import_audio);growing.waitJob();assert(growing.display(message)=="WAV IMPORTED");
        growing.render(128);hostTime.flags|=kVstTransportPlaying;growing.render(4410);
        rlo_test_conform_delay(30);hostTime.cycleEndPos=16;growing.render(128);
        int64_t expectedSample=4410+128;
        for(int n=0;n<100;++n){growing.render(128);for(int i=0;i<128;++i){const float expected=.125f*std::sin(6.283185307179586*217*(expectedSample+i)/44100);if(std::abs(growing.lastLeft[i]-expected)>2.e-6f)std::cerr<<"Length-grow discontinuity at "<<expectedSample+i<<": "<<growing.lastLeft[i]<<" expected "<<expected<<"\n";assert(std::abs(growing.lastLeft[i]-expected)<2.e-6f);}expectedSample+=128;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        rlo_test_conform_delay(0);assert(growing.display(duration)=="8.00 SEC");assert(rlo_test_fallback_frames(growing.fx)>0);
        const auto beforeReverse=growing.save();const auto beforeFallback=rlo_test_fallback_frames(growing.fx);
        const double reverseStart=growing.fx->getParameter(growing.fx,position);int reverseRendered=0;
        rlo_test_conform_delay(30);growing.tap(reverse);
        for(int n=0;n<150;++n){growing.render(128);reverseRendered+=128;assert(growing.rms()>.03&&"Live Reverse must not interrupt the loop while its worker holds the engine lock");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        growing.waitJob();rlo_test_conform_delay(0);growing.render(128);reverseRendered+=128;
        assert(growing.display(message)=="EDIT COMPLETE"&&growing.display(status)=="PLAYING"&&rlo_test_fallback_frames(growing.fx)>beforeFallback);
        const double expectedReversePhase=std::fmod(reverseStart+reverseRendered/352800.,1.);
        double phaseError=growing.fx->getParameter(growing.fx,position)-expectedReversePhase;phaseError-=std::round(phaseError);
        assert(std::abs(phaseError)<.001&&"Reverse must preserve the MPC sequence phase");
        const auto afterReverse=growing.save();const auto reverseAudioAt=44+param_count*4+4+4*midiTargetCount*4;
        for(int channel=0;channel<2;++channel)for(int i=0;i<352800;++i){float expected,actual;
            std::memcpy(&expected,beforeReverse.data()+reverseAudioAt+(channel*352800+352799-i)*4,4);
            std::memcpy(&actual,afterReverse.data()+reverseAudioAt+(channel*352800+i)*4,4);assert(actual==expected);
        }
        growing.tap(undo);growing.render(128);const auto undoReverse=growing.save();
        assert(std::memcmp(beforeReverse.data()+reverseAudioAt,undoReverse.data()+reverseAudioAt,352800*8)==0);
        growing.tap(redo);growing.render(128);const auto redoReverse=growing.save();
        assert(std::memcmp(afterReverse.data()+reverseAudioAt,redoReverse.data()+reverseAudioAt,352800*8)==0);
        hostTime.flags&=~kVstTransportPlaying;hostTime.ppqPos=0;hostTime.cycleEndPos=8;
        Host partial(false);partial.render(128);std::this_thread::sleep_for(std::chrono::milliseconds(100));partial.tap(record);partial.render(128);
        hostTime.flags|=kVstTransportPlaying;partial.render(107604,.1,217);hostTime.flags&=~kVstTransportPlaying;
        waitLength(partial,"4.00 SEC");const auto padded=partial.save();assert(u32(padded.data()+20)==176400);
        const auto audioAt=44+param_count*4+4+4*midiTargetCount*4;
        for(int i=107604;i<176400;++i){float sample;std::memcpy(&sample,padded.data()+audioAt+i*4,4);assert(sample==0&&"An incomplete first take must pad silence, not repeat off-grid audio");}
        supplyTime=false;
        std::cout<<"PASS: multi-bar audio and jitter stability; sequence/next-bar phase; automatic stopped/playing length matching; shortened-source persistence and restoration; continuous playback through forced 30ms worker allocation\n";
    }
    {
        retrolooper::LooperEngine e;e.prepare(44100);e.reserveCaptureStorage(512);
        std::array<float,64> left{},right{};for(int i=0;i<64;++i){left[i]=i*.01f-.3f;right[i]=.2f-i*.005f;}
        const float* input[]{left.data(),right.data()};assert(e.importAudioFile(0,input,2,64,true));
        auto& rotated=const_cast<retrolooper::Track&>(e.tracks()[0]);assert(rotated.rotateLeft(13));
        std::array<std::array<float,64>,2> before{};for(int c=0;c<2;++c)for(int i=0;i<64;++i)before[c][i]=rotated.readSample(c,i);
        assert(e.hostReverseAtCurrentPhase());for(int c=0;c<2;++c)for(int i=0;i<64;++i)assert(rotated.readSample(c,i)==before[c][63-i]);
        assert(e.hostReverseAtCurrentPhase());for(int c=0;c<2;++c)for(int i=0;i<64;++i)assert(rotated.readSample(c,i)==before[c][i]);
    }
    {
        for(double sr:{44100.,48000.}){
            retrolooper::LooperEngine e;e.prepare(sr);
            for(const auto& t:e.tracks())assert(t.allocatedCapacity()==0);
            assert(e.captureStorageCapacity()==0);
            const auto shortTake=static_cast<std::size_t>(sr*4)+512;
            e.reserveCaptureStorage(shortTake);assert(e.captureStorageCapacity()==shortTake);
            assert(e.tracks()[0].allocatedCapacity()==shortTake);
            for(int i=1;i<e.kMaxTracks;++i)assert(e.tracks()[i].allocatedCapacity()==0);
            e.reserveCaptureStorage(shortTake/2);assert(e.captureStorageCapacity()==shortTake);
            e.reserveCaptureStorage(shortTake*2);assert(e.captureStorageCapacity()==shortTake*2);
        }
        std::cout<<"PASS: empty preparation reserves no audio/history; exact-sized stereo storage at 44.1/48 kHz, unused tracks empty, growth and reuse\n";
        assert(captureBudgetFits(0,44100*8,256u*1024*1024));
        assert(!captureBudgetFits(0,44100*120,256u*1024*1024));
        assert(!captureBudgetFits(0,44100*8,64u*1024*1024));
        assert(captureBudgetFits(44100*8,44100*8,0));
    }
    {
        CassetteTransport tape;
        const double fill=tape.fill;const double dt=.2;
        tape.advance(dt,true);
        const double mid=(fill+tape.fill)*.5;
        assert(tape.takeupPhase>tape.supplyPhase&&tape.supplyPhase>0);
        const double supplySpeed=tape.supplyPhase*CassetteTransport::tau*CassetteTransport::radius(1-mid)/dt;
        const double takeupSpeed=tape.takeupPhase*CassetteTransport::tau*CassetteTransport::radius(mid)/dt;
        assert(std::abs(supplySpeed-CassetteTransport::tapeSpeed)<1.e-12);
        assert(std::abs(supplySpeed-takeupSpeed)<1.e-12);
        const auto frozen=tape;tape.advance(10,false);
        assert(tape.supplyPhase==frozen.supplyPhase&&tape.takeupPhase==frozen.takeupPhase&&tape.fill==frozen.fill);
        for(int i=0;i<1000;++i){tape.advance(.1,true);assert(tape.supplyPhase>=0&&tape.supplyPhase<1&&tape.takeupPhase>=0&&tape.takeupPhase<1);}
        const double area=std::pow(CassetteTransport::radius(tape.fill),2)+std::pow(CassetteTransport::radius(1-tape.fill),2);
        assert(std::abs(area-(std::pow(CassetteTransport::coreRadius,2)+std::pow(CassetteTransport::outerRadius,2)))<1.e-12);
        Host moving;moving.set(view,1);moving.recordTone();
        const float startFill=moving.fx->getParameter(moving.fx,tape_fill);
        moving.render(44100*3);
        assert(moving.fx->getParameter(moving.fx,tape_fill)>startFill); // 3 loop wraps never rewind tape.
        moving.tap(mute);moving.render(4410);const float muted=moving.fx->getParameter(moving.fx,takeup_phase);
        moving.render(11025);assert(moving.fx->getParameter(moving.fx,takeup_phase)!=muted);
        moving.stopLoop();const float a=moving.fx->getParameter(moving.fx,reel_phase),b=moving.fx->getParameter(moving.fx,takeup_phase),f=moving.fx->getParameter(moving.fx,tape_fill);
        moving.render(44100);assert(moving.fx->getParameter(moving.fx,reel_phase)==a&&moving.fx->getParameter(moving.fx,takeup_phase)==b&&moving.fx->getParameter(moving.fx,tape_fill)==f);
        std::cout<<"PASS: cassette equal linear tape speed, independent reel rates, conserved pack area, continuous loop wraps, mute movement and stopped freeze\n";
    }
    {
        Host visual;
        visual.tap(record);visual.render(44100,.25);visual.render(44100,0);visual.tap(record);visual.render(128);
        assert(visual.fx->getParameter(visual.fx,wave_hi_00)>.1f);
        assert(visual.fx->getParameter(visual.fx,wave_lo_00)>.1f);
        assert(visual.fx->getParameter(visual.fx,wave_hi_47)==0);
        assert(visual.fx->getParameter(visual.fx,wave_lo_47)==0);
        visual.set(view,1);const float phase=visual.fx->getParameter(visual.fx,reel_phase);visual.render(11025);
        assert(std::abs(visual.fx->getParameter(visual.fx,reel_phase)-phase)>.01f);
        // Cassette now shows all 48 columns, with always-visible stereo side meters.
        reports.fill(0);visual.set(view,0);visual.render(4410,.18);
        reports.fill(0);visual.set(view,1);visual.render(4410,.24);
        assert(reports[wave_hi_01]>0&&reports[input_peak_l]>0&&reports[output_peak_l]>0);
        auto selected=visual.save();assert(visual.load(selected));assert(visual.fx->getParameter(visual.fx,view)==.5f);
        // v0.1.3/0.1.4 chunks have 167 parameters and all 51 MIDI targets.
        auto prior=selected;prior.erase(prior.begin()+44+167*4,prior.begin()+44+param_count*4);
        const uint32_t priorCount=167;std::memcpy(prior.data()+8,&priorCount,4);
        uint32_t priorHash=2166136261u;for(std::size_t i=0;i<prior.size()-4;++i)priorHash=(priorHash^prior[i])*16777619u;
        std::memcpy(prior.data()+prior.size()-4,&priorHash,4);
        assert(visual.load(prior));assert(visual.fx->getParameter(visual.fx,view)==.5f);assert(visual.display(binding)=="NOTE 36 / CC 20");
        const float frozen=visual.fx->getParameter(visual.fx,reel_phase);visual.render(22050);
        assert(visual.fx->getParameter(visual.fx,reel_phase)==frozen);
        visual.set(view,2);visual.tap(play);reports.fill(0);visual.render(44100,.2);
        assert(visual.fx->getParameter(visual.fx,input_peak_l)>visual.fx->getParameter(visual.fx,input_peak_r));
        assert(visual.fx->getParameter(visual.fx,input_rms_l)>visual.fx->getParameter(visual.fx,input_rms_r));
        assert(reports[input_peak_l]>0&&reports[wave_hi_00]==0&&reports[reel_phase]==0);
        auto meters=visual.save();assert(visual.load(meters));assert(visual.fx->getParameter(visual.fx,view)==1.f);
        // Build a v1 fixture from a current chunk: old params and the original 21 note/7 CC slots.
        for(const uint32_t count:{56u,162u}) {
            std::vector<uint8_t> legacy(selected.begin(),selected.begin()+44+count*4);
            const uint32_t version=1;std::memcpy(legacy.data()+4,&version,4);std::memcpy(legacy.data()+8,&count,4);
            const float oldTarget=4.f/6.f;std::memcpy(legacy.data()+44+learn_target*4,&oldTarget,4);
            const auto bindingsAt=44+param_count*4+4;
            for(int g=0;g<4;++g)legacy.insert(legacy.end(),selected.begin()+bindingsAt+g*midiTargetCount*4,selected.begin()+bindingsAt+(g*midiTargetCount+7)*4);
            legacy.insert(legacy.end(),selected.begin()+bindingsAt+4*midiTargetCount*4,selected.end()-4);
            uint32_t checksum=2166136261u;for(auto x:legacy)checksum=(checksum^x)*16777619u;
            const auto* bytes=reinterpret_cast<const uint8_t*>(&checksum);legacy.insert(legacy.end(),bytes,bytes+4);
            assert(visual.load(legacy));assert(visual.fx->getParameter(visual.fx,view)==(count==56?0.f:.5f));
            assert(visual.display(learn_target)=="Undo");assert(visual.display(binding)=="NOTE 40 / CC 24");
            assert(visual.display(status)=="STOPPED");assert(visual.fx->getParameter(visual.fx,wave_hi_00)>.1f);
        }
        visual.tap(clear);visual.render(128);
        for(int p=wave_lo_00;p<=wave_hi_47;++p)assert(visual.fx->getParameter(visual.fx,p)==0);
        std::cout<<"PASS: complete signed waveform, silent region, animated/frozen cassette reels, stereo peak/RMS levels, hidden-view reporting, saved view selection, legacy project migration, cleared visuals\n";
    }
    {
        Host mapped;
        auto target=[](int parameter){for(int i=0;i<midiVisibleCount;++i)if(midiTargets[midiVisibleTargets[i]]==parameter)return i;assert(false);return -1;};
        auto learnCC=[&](int parameter,int cc,int initial=0){mapped.set(learn_target,target(parameter));mapped.set(learn,1);mapped.midi(0xb0,cc,initial);mapped.render(128);assert(mapped.fx->getParameter(mapped.fx,learn)==0);};
        auto learnNote=[&](int parameter,int note,int gestureKind=0){mapped.set(learn_target,target(parameter));mapped.set(gesture,gestureKind);mapped.set(learn,1);mapped.midi(0x90,note,127);mapped.render(128);mapped.midi(0x80,note,0);mapped.render(128);};
        // Every interactive parameter is discoverable, learnable and survives a saved project.
        for(int i=0;i<midiVisibleCount;++i){learnNote(midiTargets[midiVisibleTargets[i]],70+i);learnCC(midiTargets[midiVisibleTargets[i]],40+i);}
        auto fullMap=mapped.save();Host restored;assert(restored.load(fullMap));
        for(int i=0;i<midiVisibleCount;++i){restored.set(learn_target,i);restored.set(gesture,0);assert(restored.display(binding)=="NOTE "+std::to_string(70+i)+" / CC "+std::to_string(40+i));}
        learnCC(volume,7);mapped.set(volume,1);mapped.set(monitor,1);
        mapped.midi(0xb0,7,0,64);mapped.render(128,.2);
        double before=0;for(int i=0;i<64;++i)before+=std::abs(mapped.lastLeft[i]);assert(before>1);
        for(int i=64;i<128;++i)assert(mapped.lastLeft[i]==0); // CC changes at the event, never at block start.
        mapped.midi(0xb0,7,127,96);mapped.render(128,.2);
        for(int i=0;i<96;++i)assert(mapped.lastLeft[i]==0);assert(std::abs(mapped.lastLeft[127])>0);
        learnCC(pan,8);mapped.midi(0xb0,8,0);mapped.render(128);assert(mapped.display(pan)=="100 L");
        mapped.midi(0xb0,8,127);mapped.render(128);assert(mapped.display(pan)=="100 R");
        learnNote(launch_mode,90);mapped.set(launch_mode,0);mapped.midi(0x90,90,127);mapped.render(128);assert(mapped.display(launch_mode)=="Next bar");mapped.set(launch_mode,0);
        learnNote(view,91);mapped.set(view,0);mapped.midi(0x90,91,127);mapped.render(128);assert(mapped.display(view)=="Cassette");
        learnNote(bars,92);mapped.set(bars,4);mapped.midi(0x90,92,127);mapped.render(128);assert(mapped.display(bars)=="5 bars");
        learnCC(input,9);mapped.midi(0xb0,9,127);mapped.render(128);assert(mapped.display(input)=="Off");mapped.set(input,0);
        mapped.set(host_sync,0);mapped.set(monitor,0);mapped.set(pan,0);mapped.recordTone();
        learnCC(stop_now,10);mapped.midi(0xb0,10,127);mapped.render(128);assert(mapped.display(status)=="STOPPED");
        mapped.midi(0xb0,10,127);mapped.render(128);assert(mapped.display(status)=="STOPPED");
        learnNote(reverse,93);mapped.midi(0x90,93,127);mapped.render(128);mapped.waitJob();assert(mapped.display(message)=="EDIT COMPLETE");
        mapped.tap(play);mapped.render(128);mapped.midi(0x90,93,127);mapped.render(128);mapped.waitJob();assert(mapped.display(message)=="EDIT COMPLETE"&&mapped.display(status)=="PLAYING");
        mapped.set(learn_target,target(reverse));mapped.tap(forget);assert(mapped.display(binding)=="NOTE -1 / CC -1");
        learnNote(view,94,2);mapped.set(view,0);mapped.midi(0x90,94,127);mapped.render(22000);assert(mapped.display(view)=="Waveform");mapped.render(256);assert(mapped.display(view)=="Cassette");mapped.midi(0x80,94,0);mapped.render(128);
        learnNote(view,95,1);mapped.set(view,0);for(int tap=0;tap<2;++tap){mapped.midi(0x90,95,127);mapped.render(128);mapped.midi(0x80,95,0);mapped.render(128);}assert(mapped.display(view)=="Cassette");
        std::cout<<"PASS: all "<<midiVisibleCount<<" visible controls learn notes/CC and save/restore; CC sample offsets, full-range pan/routing, note toggle/cycle/integer, mapped stop/reverse guard/forget, hold/double tap\n";
    }
    {
        Host meter;meter.set(view,2);meter.render(4410,1.2);
        assert(meter.fx->getParameter(meter.fx,focus_clip)==1);
        const float high=meter.fx->getParameter(meter.fx,focus_meter);assert(high>.99f);
        meter.render(22050);const float level=meter.fx->getParameter(meter.fx,focus_meter),hold=meter.fx->getParameter(meter.fx,focus_hold);
        assert(hold>level&&hold<high);assert(meter.fx->getParameter(meter.fx,focus_clip)==1);
        meter.render(110250);assert(meter.fx->getParameter(meter.fx,focus_clip)==0);
        std::cout<<"PASS: main-app meter peak release, slower hold and 2.2-second clip latch\n";
    }
    {
        // MPC sends one touch as two highs, even after our 100 ms spring-back.
        Host touch;touch.set(host_sync,0);touch.set(record,1);touch.render(6000,.25);
        assert(touch.display(status)=="RECORDING");
        std::this_thread::sleep_for(std::chrono::milliseconds(140));
        touch.set(record,1);touch.render(128,.25);
        assert(touch.display(status)=="RECORDING" && "One native touchscreen tap must not close its own recording");
        // A release/echo cannot turn the second high into a second tap either.
        touch.set(record,0);touch.set(record,1);touch.render(128,.25);
        assert(touch.display(status)=="RECORDING");
        std::this_thread::sleep_for(std::chrono::milliseconds(130));
        touch.set(record,0);touch.set(record,1);touch.set(record,0);touch.render(128);
        assert(touch.display(status)=="PLAYING");
        touch.render(44100);assert(touch.rms()>.1);
        std::cout<<"PASS: MPC duplicate touch-high after spring-back and release echo ignored; next deliberate Record tap closes a playable take\n";
    }
    {
        // An insert receives its own channel buffers and returns to that same
        // channel. Distinct signed DC channels expose accidental summing/swaps.
        Host channel;channel.set(host_sync,0);channel.set(monitor,1);
        std::array<float,128> left{},right{},outL{},outR{};float* in[]{left.data(),right.data()};float* out[]{outL.data(),outR.data()};
        auto block=[&](float l,float r){left.fill(l);right.fill(r);channel.fx->processReplacing(channel.fx,in,out,128);};
        block(.3f,-.15f);
        for(int i=0;i<128;++i){assert(std::abs(outL[i]-.3f)<1.e-6f);assert(std::abs(outR[i]+.15f)<1.e-6f);}
        channel.set(monitor,0);channel.tap(record);
        for(int i=0;i<350;++i)block(.3f,-.15f);
        channel.tap(record);for(int i=0;i<20;++i)block(0,0);
        assert(channel.display(status)=="PLAYING");
        for(int i=0;i<128;++i){assert(std::abs(outL[i]-.3f)<1.e-5f);assert(std::abs(outR[i]+.15f)<1.e-5f);}
        channel.set(output,1);block(.8f,.6f);for(int i=0;i<128;++i)assert(outL[i]==0&&outR[i]==0);
        channel.set(output,0);block(0,0);assert(outL[100]>.29f&&outR[100]<-.14f);
        channel.stopLoop();block(0,0);assert(outL[100]==0&&outR[100]==0);
        std::cout<<"PASS: unity stereo channel monitoring, independent L/R capture, zero-input playback to the same insert outputs, output-off and stop\n";
    }
    {
        // Actual MPC protocol: PPQ, tempo, cycle marks and transport all supplied
        // by the host; no independent plugin timer determines sequence length.
        for(int ratio=0;ratio<3;++ratio){
            supplyTime=true;hostTime={};hostTime.tempo=120;hostTime.sampleRate=44100;
            hostTime.timeSigNumerator=hostTime.timeSigDenominator=4;
            hostTime.cycleStartPos=0;hostTime.cycleEndPos=4;
            hostTime.flags=kVstTempoValid|kVstPpqPosValid|kVstTimeSigValid|kVstCyclePosValid|kVstTransportCycleActive;
            Host synced(false);synced.set(host_sync,1);synced.set(length_ratio,ratio);synced.set(click,1);synced.set(click_gain,1);
            synced.tap(record);synced.render(44100,.25);
            assert(synced.display(status)=="ARMED");assert(synced.display(duration)=="0.00 SEC");
            assert(synced.display(sequence_info).find("SEQ 1")!=std::string::npos);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            hostTime.ppqPos=0;hostTime.flags|=kVstTransportPlaying;
            const int target=ratio==0?44100:ratio==1?88200:176400;
            if(ratio==2){synced.render(88320,.25);assert(synced.display(status)=="RECORDING");synced.render(target-88320,.25);}
            else synced.render(target,.25);
            synced.render(128);assert(synced.display(status)=="PLAYING");
            assert(u32(synced.save().data()+20)==static_cast<uint32_t>(target));
            // Clock jitter and regular sequence wraps never stop playback.
            for(int i=0;i<1000;++i){hostTime.ppqPos+=i%2?.0006:-.0006;synced.render(128);assert(synced.display(status)=="PLAYING");}
            hostTime.flags&=~kVstTransportPlaying;synced.render(512);assert(synced.display(status)=="STOPPED");assert(synced.rms()==0);
            synced.tap(play);synced.render(512);assert(synced.display(status)=="STOPPED");
            hostTime.ppqPos=0;hostTime.flags|=kVstTransportPlaying;synced.render(4410);assert(synced.display(status)=="PLAYING");assert(synced.rms()>.1);
            // MPC channel monitoring passes at unity even with plugin monitor off,
            // record input off, input/loop/master gains changed, loop output off.
            hostTime.flags&=~kVstTransportPlaying;synced.set(monitor,0);synced.set(input,3);synced.set(input_gain,4);synced.set(volume,0);synced.set(master,0);synced.set(output,1);
            synced.render(1024,.2);assert(synced.rms()>.13&&synced.rms()<.15);
            synced.render(4410);assert(synced.rms()==0); // no independent click
        }
        // The first take requires explicit Record; MPC REC/OVERDUB owns later layers.
        supplyTime=true;hostTime={};hostTime.tempo=120;hostTime.sampleRate=44100;hostTime.timeSigNumerator=hostTime.timeSigDenominator=4;
        hostTime.cycleEndPos=4;hostTime.flags=kVstTempoValid|kVstPpqPosValid|kVstTimeSigValid|kVstCyclePosValid|kVstTransportCycleActive;
        Host automatic(false);automatic.set(host_sync,1);automatic.render(128);assert(automatic.display(status)=="EMPTY");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        hostTime.flags|=kVstTransportPlaying|kVstTransportRecording;hostTime.ppqPos=0;
        automatic.render(88328,.2);assert(automatic.display(status)=="EMPTY");assert(u32(automatic.save().data()+20)==0);
        ++physicalRecordPresses;automatic.render(88200*2,.2);automatic.render(128);
        assert(automatic.display(status)=="OVERDUB");assert(u32(automatic.save().data()+20)==88200);
        automatic.render(88200,.1);hostTime.flags&=~kVstTransportRecording;automatic.render(512);
        assert(automatic.display(status)=="PLAYING");automatic.render(44100);assert(automatic.rms()>.17);
        hostTime.flags|=kVstTransportRecording;automatic.render(512,.1);assert(automatic.display(status)=="OVERDUB");
        hostTime.flags&=~(kVstTransportPlaying|kVstTransportRecording);
        // Replay MPC's parameter-zero probe on STOP; it must not arm Record.
        automatic.fx->dispatcher(automatic.fx,effMainsChanged,0,0,nullptr,0);
        automatic.fx->dispatcher(automatic.fx,effMainsChanged,0,1,nullptr,0);
        automatic.set(record,1);automatic.set(record,0);automatic.render(512);
        assert(automatic.display(status)=="STOPPED");assert(automatic.rms()==0);
        hostTime.flags|=kVstTransportPlaying;hostTime.ppqPos=0;automatic.render(4410);assert(automatic.display(status)=="PLAYING");
        std::cout<<"PASS: MPC Play/Rec/Overdub cannot create an empty take; physical REC arms the first take, MPC continues audio overdubbing, Record-off returns to playback, Overdub resumes, MPC Stop probe cannot rearm, Play resumes stored audio\n";
        // Exercise all history layers with only one sequence worth of storage,
        // rather than the old 120-second allocation used by standalone tests.
        hostTime.flags&=~(kVstTransportPlaying|kVstTransportRecording);hostTime.ppqPos=0;
        {
            Host compact(false);compact.render(128);std::this_thread::sleep_for(std::chrono::milliseconds(50));
            compact.tap(record);compact.render(128);hostTime.flags|=kVstTransportPlaying;
            compact.render(88200,.2);compact.render(128);
            if(compact.display(status)!="PLAYING")std::cerr<<"Compact capture: "<<compact.display(status)<<" / "<<compact.display(message)<<" / "<<compact.display(duration)<<" ppq="<<hostTime.ppqPos<<"\n";
            assert(compact.display(status)=="PLAYING");
            compact.render(88200);const double original=compact.rms();
            for(int layer=0;layer<4;++layer){compact.tap(dub);compact.render(88200,.04,550+layer*110);compact.tap(dub);compact.render(128);}
            compact.render(88200);const double layered=compact.rms();assert(layered>original);
            for(int layer=0;layer<4;++layer){compact.tap(undo);compact.render(128);}
            compact.render(88200);assert(std::abs(compact.rms()-original)<.003);
            for(int layer=0;layer<4;++layer){compact.tap(redo);compact.render(128);}
            compact.render(88200);assert(std::abs(compact.rms()-layered)<.003);
            const auto saved=compact.save();Host recalled(false);assert(recalled.load(saved));
            assert(recalled.display(status)=="STOPPED");
            assert(u32(saved.data()+20)==88200);
        }
        std::cout<<"PASS: four overdub undo/redo levels and project recall using compact sequence-sized storage\n";
        std::cout<<"PASS: MPC start/stop owns loop transport; sequence /2, x1, x2 close at exact frame counts, x2 spans wraps, jitter tolerated, channel monitoring unity and independent click suppressed\n";
    }
    {
        for(bool noSymbols:{true,false}){
            supplyTime=true;hostTime={};hostTime.tempo=120;hostTime.flags=kVstTempoValid|kVstPpqPosValid;
            bridgeSupported=false;
            setenv("RETROLOOPER_TEST_TRACK_GUARD","1",1);
            if(noSymbols)setenv("RETROLOOPER_TEST_NO_CONTROLS","1",1);
            Host portable(false),other(false);
            unsetenv("RETROLOOPER_TEST_NO_CONTROLS");unsetenv("RETROLOOPER_TEST_TRACK_GUARD");
            portable.set(bars,1);
            for(int i=0;i<100&&portable.display(sequence_info)!="TAKE 1 B";++i){
                portable.render(128);std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            assert(portable.display(capture_source)=="Touch/Pads"&&portable.display(sequence_info)=="TAKE 1 B");
            assert(portable.display(pad_status)=="TOUCH PADS / MIDI");
            std::this_thread::sleep_for(std::chrono::milliseconds(130));portable.render(128);
            ++physicalRecordPresses;
            hostTime.flags|=kVstTransportRecording;
            portable.render(128,.2f);assert(portable.display(status)=="EMPTY");
            portable.set(pad_bank_enabled,0); // Hardware setting does not disable on-screen pads.
            portable.tap(pad_press_01);portable.render(128);assert(portable.display(status)=="ARMED");
            hostTime.flags|=kVstTransportPlaying;portable.render(88200,.2f);portable.render(128);
            assert(portable.display(status)=="PLAYING"&&u32(portable.save().data()+20)==88200);
            assert(other.display(status)=="EMPTY");
            portable.tap(dub);portable.render(128,.1f);assert(portable.display(status)=="OVERDUB");
            portable.tap(dub);portable.render(128);assert(portable.display(status)=="PLAYING");
#ifdef RETROLOOPER_FORCE_BOUNCE
            portable.tap(bounce_clip);assert(portable.display(message)=="FORCE CONTROLS NEEDED");
#endif
            hostTime.flags&=~kVstTransportPlaying;portable.render(128);assert(portable.display(status)=="STOPPED");
            bridgeSupported=true;supplyTime=false;
        }
        std::cout<<"PASS: absent or incompatible Controls: manual take length on host clock, touch pads, local overdub and independent instances\n";
    }
    {
        // A real v0.1.8 state has 169 params and 51 mapping targets. The old
        // selected-target value uses its original 0..50 range, not the new one.
        Host original;original.recordTone();auto full=original.save();
        std::vector<uint8_t> old(full.begin(),full.begin()+44+169*4);uint32_t count=169,targets=51;
        std::memcpy(old.data()+8,&count,4);float selected=1;std::memcpy(old.data()+44+learn_target*4,&selected,4);
        const auto* tbytes=reinterpret_cast<uint8_t*>(&targets);old.insert(old.end(),tbytes,tbytes+4);
        const auto bindings=44+param_count*4+4;
        for(int g=0;g<4;++g)old.insert(old.end(),full.begin()+bindings+g*midiTargetCount*4,full.begin()+bindings+(g*midiTargetCount+51)*4);
        old.insert(old.end(),full.begin()+bindings+4*midiTargetCount*4,full.end()-4);
        uint32_t hash=2166136261u;for(auto x:old)hash=(hash^x)*16777619u;const auto* hbytes=reinterpret_cast<uint8_t*>(&hash);old.insert(old.end(),hbytes,hbytes+4);
        Host migrated;assert(migrated.load(old));assert(migrated.display(learn_target)=="Next MIDI Target");assert(migrated.display(length_ratio)=="x1");assert(migrated.display(capture_source)=="Touch/Pads");assert(u32(migrated.save().data()+20)==44100);
        std::cout<<"PASS: v0.1.8 169-parameter/51-target project migration preserves audio, target selection and default MPC capture mode\n";
    }
    {
        Host animation;assert(animation.fx->dispatcher(animation.fx,effCanBeAutomated,position,0,nullptr,0)==1);
        const float before=animation.fx->getParameter(animation.fx,position);animation.fx->setParameter(animation.fx,position,.9f);assert(animation.fx->getParameter(animation.fx,position)==before);
        animation.set(view,1);animation.recordTone();std::this_thread::sleep_for(std::chrono::milliseconds(210));reports.fill(0);displayReports=0;animation.render(44100);
        assert(reports[position]>=25&&reports[position]<=31);assert(reports[reel_phase]>=25&&reports[reel_phase]<=31);
        assert(displayReports<=2); // value-driven animation never rebuilds the native program/preset screen periodically
        animation.set(volume,.8f);displayReports=0;animation.render(22050);assert(displayReports<=1); // no redraw for an ordinary touch/Q-Link write
        animation.stopLoop();animation.render(4410);reports.fill(0);animation.render(44100);assert(reports[position]==0&&reports[reel_phase]==0);
        std::cout<<"PASS: playhead/reels near 30 Hz through live host bindings; no periodic native screen rebuilds; controls protected and stopped motion frozen\n";
    }
    Host a,b;assert(a.fx->dispatcher(a.fx,effGetTailSize,0,0,nullptr,0)==INT32_MAX);a.recordTone();a.render(44100);const auto clean=a.rms();assert(clean>.1&&clean<.3);b.render(512);assert(b.rms()==0);assert(b.display(status)=="EMPTY");
    a.tap(dub);a.render(44100,.1,880);a.tap(dub);a.render(128);assert(a.display(status)=="PLAYING");a.render(44100);const auto overdubbed=a.rms();assert(overdubbed>clean);
    a.tap(undo);a.render(44100);assert(std::abs(a.rms()-clean)<.01);a.tap(redo);a.render(44100);assert(a.rms()>clean);
    auto state=a.save();assert(b.load(state));assert(b.display(status)=="STOPPED");b.render(512);assert(b.rms()<.001);b.tap(play);b.render(44100);assert(b.rms()>.1);
    auto previousVersion=state;
    previousVersion.resize(44+183*4);const uint32_t previousTargets=58;
    const auto appendOld=[&](const void* p,std::size_t n){const auto* b=static_cast<const uint8_t*>(p);previousVersion.insert(previousVersion.end(),b,b+n);};
    appendOld(&previousTargets,4);for(int group=0;group<4;++group)appendOld(state.data()+44+param_count*4+4+group*midiTargetCount*4,previousTargets*4);
    appendOld(state.data()+44+param_count*4+4+4*midiTargetCount*4,state.size()-(44+param_count*4+4+4*midiTargetCount*4));
    const uint32_t previousCount=183;std::memcpy(previousVersion.data()+8,&previousCount,4);
    uint32_t previousHash=2166136261u;for(std::size_t i=0;i+4<previousVersion.size();++i)previousHash=(previousHash^previousVersion[i])*16777619u;
    std::memcpy(previousVersion.data()+previousVersion.size()-4,&previousHash,4);
    {Host recall;assert(recall.load(previousVersion));assert(recall.display(duration)==a.display(duration));recall.tap(play);recall.render(44100);assert(recall.rms()>.1);}
    auto broken=state;broken[80]^=0x80;assert(!b.load(broken));assert(b.display(status)=="PLAYING");
    for(std::size_t n:{0u,1u,20u,100u})assert(!b.load(std::vector<uint8_t>(n,0xff)));
    a.stopLoop();a.tap(reverse);a.waitJob();assert(a.display(message)=="EDIT COMPLETE");
    a.set(file_slot,61);const auto directory=std::getenv("RETROLOOPER_TEST_LOOPS");assert(directory);std::filesystem::remove(std::filesystem::path(directory)/"Loop-61.wav");std::filesystem::remove(std::filesystem::path(directory)/"Loop-62.wav");
    a.tap(export_audio);a.waitJob();assert(a.display(message)=="WAV EXPORTED");
#ifdef RETROLOOPER_FORCE_BOUNCE
    {
        static_assert(bounce_clip==266&&record_offset==270&&midiTargetCount==63);
        Host empty;empty.tap(bounce_clip);assert(empty.display(message)=="NO LOOP AUDIO"&&clipSubmissions==0);
        clipRow=0;a.tap(bounce_clip);assert(a.display(message)=="PICK PAD OR MATRIX"&&clipSubmissions==0);
        const float lamp=a.fx->getParameter(a.fx,clip_indicator);
        for(int i=0;i<70&&a.fx->getParameter(a.fx,clip_indicator)==lamp;++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        assert(a.fx->getParameter(a.fx,clip_indicator)!=lamp&&clipSubmissions==0);
        a.tap(bounce_clip);
        for(int i=0;i<100&&pickerOwner.load();++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));
        assert(!pickerOwner.load()&&clipSubmissions==0);
        a.tap(bounce_clip);clipRow=2;
        for(int i=0;i<200&&a.display(message)!="BOUNCED TO CLIP";++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));
        assert(a.display(message)=="BOUNCED TO CLIP"&&clipSubmissions==1);
        const auto file=readWave(bouncedPath);
        assert(bouncedPath.filename().string().starts_with("Bounce-")&&file.rate==44100&&file.left.size()==44100);
        assert(std::any_of(file.left.begin(),file.left.end(),[](float v){return std::abs(v)>.1f;}));
        assert(a.display(file_slot)=="Slot 61");
        std::filesystem::remove(bouncedPath);
        a.set(record_offset,11);
        auto saved=a.save();
        const int viewAction=1+static_cast<int>(std::find_if(midiVisibleTargets,midiVisibleTargets+midiVisibleCount,[](int i){return midiTargets[i]==view;})-midiVisibleTargets);
        for(const auto [oldCount,oldTargets]:{std::pair{266u,59u},std::pair{270u,62u},std::pair{271u,63u}}){
            std::vector<uint8_t> old(saved.begin(),saved.begin()+44+oldCount*4);
            std::memcpy(old.data()+8,&oldCount,4);
            const float oldPad=viewAction/static_cast<float>(oldTargets-7);
            std::memcpy(old.data()+44+pad_a_14*4,&oldPad,4);
            const auto* n=reinterpret_cast<const uint8_t*>(&oldTargets);
            old.insert(old.end(),n,n+4);
            const auto bindings=44+param_count*4+4;
            for(int g=0;g<4;++g)old.insert(old.end(),saved.begin()+bindings+g*midiTargetCount*4,saved.begin()+bindings+(g*midiTargetCount+oldTargets)*4);
            old.insert(old.end(),saved.begin()+bindings+4*midiTargetCount*4,saved.end()-4);
            uint32_t hash=2166136261u;for(auto v:old)hash=(hash^v)*16777619u;
            const auto* sum=reinterpret_cast<const uint8_t*>(&hash);old.insert(old.end(),sum,sum+4);
            Host migrated;assert(migrated.load(old));assert(migrated.display(pad_a_14)=="View");assert(migrated.display(duration)==a.display(duration));
            assert(migrated.display(record_offset)==(oldCount==271?"11 ms":"Auto (7 ms)"));
        }
        std::cout<<"PASS: Force Clip blinks, cancels and bounces immediately after a pick, preserves WAV slots, and recalls earlier audio, pads and offset\n";
    }
#else
    {
        timelineAvailable=true;
        a.tap(export_audio);a.waitJob();assert(a.display(message)=="BOUNCED TO TRACK");
        const auto file=readWave(timelinePath);
        assert(file.rate==44100&&file.left.size()==44100&&a.display(file_slot)=="Slot 61");
        std::filesystem::remove(timelinePath);
        timelineResult=RLO_TIMELINE_OCCUPIED;
        a.tap(export_audio);a.waitJob();assert(a.display(message)=="OVERLAP | WAV SAVED"&&waveExists(timelinePath));
        std::filesystem::remove(timelinePath);
        timelineAvailable=false;
        std::cout<<"PASS: MPC Export dispatches the rendered loop to the current timeline target and retains a WAV on overlap\n";
    }
#endif
    a.tap(clear);a.render(128);assert(a.display(status)=="EMPTY");a.tap(import_audio);a.waitJob();assert(a.display(message)=="WAV IMPORTED");assert(a.display(status)=="STOPPED");
    const auto wav=readWave(std::filesystem::path(directory)/"Loop-61.wav");assert(wav.left.size()==44100&&wav.right.size()==44100);
    const auto firstExport=wav;
    a.tap(export_audio);a.waitJob();assert(a.display(message)=="WAV EXPORTED");assert(a.display(file_slot)=="Slot 62");
    const auto unchangedExport=readWave(std::filesystem::path(directory)/"Loop-61.wav");assert(firstExport.left==unchangedExport.left&&firstExport.right==unchangedExport.right);
    a.tap(play);a.render(512);const auto forward=a.save();
    a.tap(reverse);a.waitJob();assert(a.display(message)=="EDIT COMPLETE"&&a.display(status)=="PLAYING");
    const auto reversed=a.save();const auto reverseFrames=u32(forward.data()+20);
    const auto reverseAudio=44+param_count*4+4+4*midiTargetCount*4;
    for(int c=0;c<2;++c)for(unsigned i=0;i<reverseFrames;++i){float expected,actual;
        std::memcpy(&expected,forward.data()+reverseAudio+(c*reverseFrames+reverseFrames-1-i)*4,4);
        std::memcpy(&actual,reversed.data()+reverseAudio+(c*reverseFrames+i)*4,4);
        assert(actual==expected&&"Reverse during playback must reverse both channels, not just change a label");
    }
    a.render(512);assert(a.fx->getParameter(a.fx,play_indicator)==1);
    a.tap(mute);a.render(44100);assert(a.rms()<.03&&a.fx->getParameter(a.fx,mute_indicator)==1);
    a.render(22050);assert(a.fx->getParameter(a.fx,mute_indicator)==1);
    a.tap(mute);a.render(44100);assert(a.rms()>.1&&a.fx->getParameter(a.fx,mute_indicator)==0);
    a.stopLoop();a.tap(clear);a.render(128);a.set(monitor,1);a.render(4410,.2,440,true);assert(a.rms()>.1);a.set(monitor,0);
    a.midi(0x90,36,127,17);a.render(128,.25);assert(a.display(status)=="RECORDING");a.render(44100,.25);a.midi(0x90,36,127,29);a.render(128);assert(a.display(status)=="PLAYING");
    const auto midiState=a.save();assert(u32(midiState.data()+20)==44240);
    a.stopLoop();a.set(learn_target,1);a.set(learn,1);a.midi(0x90,70,127);a.render(128);assert(a.display(status)=="STOPPED");assert(a.display(binding).find("70")!=std::string::npos);a.midi(0x80,70,0);a.render(128);a.midi(0x90,70,127);a.render(128);assert(a.display(status)=="PLAYING");
    a.stopLoop();a.set(host_sync,1);supplyTime=true;hostTime.tempo=120;hostTime.timeSigNumerator=4;hostTime.timeSigDenominator=4;hostTime.flags=kVstTempoValid|kVstPpqPosValid|kVstTimeSigValid;hostTime.ppqPos=0;
    a.render(128);assert(a.display(status)=="STOPPED");hostTime.flags|=kVstTransportPlaying;a.render(128);assert(a.display(status)=="PLAYING");hostTime.flags&=~kVstTransportPlaying;a.render(128);assert(a.display(status)=="STOPPED");
    supplyTime=false;a.set(host_sync,0);a.tap(clear);a.render(128);a.set(timing,1);a.tap(record);a.render(128,.2);assert(a.display(status)=="ARMED"||a.display(status)=="RECORDING");a.render(100000,.2);assert(a.display(status)=="RECORDING");a.tap(stop_now);a.render(128);
    a.set(timing,0);a.tap(clear);a.render(128);a.set(monitor,1);a.render(512,0,440,false,true);assert(std::abs(a.rms()-.125)<1e-6);
    a.fx->setParameter(a.fx,-1,1);a.fx->setParameter(a.fx,param_count,1);a.fx->setParameter(a.fx,volume,std::nanf(""));a.render(512);
    a.set(monitor,0);a.tap(clear);a.render(128);const auto empty=a.save();assert(b.load(empty));assert(b.display(status)=="EMPTY");
    a.tap(record);a.render(44100,.2);const auto firstPass=a.save();assert(b.load(firstPass));assert(b.display(status)=="STOPPED");a.tap(record);a.render(128);
    // Each complete overdub pass occupies a separate page-table history layer.
    a.render(44100);const double beforeFour=a.rms();
    for(int layer=0;layer<4;++layer){a.tap(dub);a.render(44100,.04,550+layer*110);a.tap(dub);a.render(128);}
    a.render(44100);const double afterFour=a.rms();assert(afterFour>beforeFour);
    for(int layer=0;layer<4;++layer){a.tap(undo);a.render(128);}
    a.render(44100);assert(std::abs(a.rms()-beforeFour)<.003);
    for(int layer=0;layer<4;++layer){a.tap(redo);a.render(128);}
    a.render(44100);assert(std::abs(a.rms()-afterFour)<.003);
    a.tap(stop_now);a.render(512);a.tap(clear);a.render(128);a.tap(record);a.render(44100*121,.05);
    assert(a.display(status)=="PLAYING");assert(a.display(message)=="120 SECOND LIMIT");assert(u32(a.save().data()+20)==44100*120);
    std::cout<<"PASS: stereo record/play, overdub, undo/redo, isolated instances, stopped state recall, corrupt chunks, reverse, WAV round trip, protected export, edit guard, mute, in-place buffers, sample-offset MIDI, MIDI learn, host start/stop, bar quantization, legacy process, invalid parameters\n";
}
