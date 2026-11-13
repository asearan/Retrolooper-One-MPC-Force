#include "Vst2.h"
#include "../control/TrackProtocol.h"
#ifdef RETROLOOPER_FORCE_BOUNCE
#include "../control/ClipProtocol.h"
#else
#include "../control/TimelineProtocol.h"
#endif
#include "Parameters.h"
#include "PadBank.h"
#include "CassetteTransport.h"
#include "CaptureMemory.h"
#include "LoopFilters.h"
#include "LoopFallback.h"
#include "WaveFile.h"
#include "LooperEngine.h"
#include "FlushDenormals.h"
#include "StereoPan.h"
#include "PluginMidiGestures.h"
#include "ui/metering/TrackMeterBallistics.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace akai {
using namespace retrolooper;
constexpr int kVersion=400,kUid=0x526c4f31,kMaxBlock=512,kStateVersion=2,kLegacyParams=56;
static std::mutex exportGate;
static std::mutex allocationGate;
static std::atomic<uint32_t> nextPadToken{1};
#ifdef RETROLOOPER_TESTING
static std::atomic<int> testConformDelay{0};
#endif
constexpr std::array<CommandType,10> commands{CommandType::RecordToggle,CommandType::PlayStopToggle,
    CommandType::OverdubToggle,CommandType::MuteToggle,CommandType::UndoOverdub,CommandType::RedoOverdub,
    CommandType::Clear,CommandType::Stop,CommandType::Stop,CommandType::SyncAllToStart};
struct Event {int offset=0;uint8_t type=0,data=0,value=0;};
struct Job {
    int index=0,slot=1,targetBars=4;uint32_t generation=0;
#ifdef RETROLOOPER_FORCE_BOUNCE
    RloForceClipTarget clipTarget{};
#else
    RloMpcTimelineTarget timelineTarget{};
#endif
};
enum Message {Ready,StopToEdit,Busy,Edited,Imported,Exported,FileError,SlotExists,Limit,QueueFull,InvalidState,Learned,NoLoop,UnsupportedRate,HostStopped,HostUnavailable,SequenceUnavailable,SequenceTooLong,SequenceArmed,PreparingAudio,MemoryLimit,MatchingLength,TrackNotSelected,TrackArmPending,TrackBridgeUnavailable,WaitingInput,FinishCapture,ManualTakeArmed,
#ifdef RETROLOOPER_FORCE_BOUNCE
    ClipBounced,SelectClip,ClipChanged,ClipStopFirst,ClipFailed,ClipSetup,ClipBusy,ClipEndCapture,ClipPickSlot,ClipCanceled,
#else
    TimelineBounced,TimelineChanged,TimelineFailed,TimelineBusy,TimelineStop,TimelineOccupied,TimelineCapture,
#endif
};
constexpr const char* messages[]{"READY TO RECORD","STOP BEFORE EDIT","WORKING...","EDIT COMPLETE","WAV IMPORTED","WAV EXPORTED",
    "WAV FILE ERROR","SLOT EXISTS; PICK NEW","120 SECOND LIMIT","CONTROL QUEUE FULL","INVALID PROJECT STATE","MIDI LEARNED","NO LOOP AUDIO","USE 44.1 OR 48 KHZ","WAIT MPC PLAY","WAITING FOR MPC CLOCK","MPC LENGTH UNAVAILABLE","SEQUENCE EXCEEDS 120S","ARMED: MPC SEQ START","PREPARING AUDIO","NOT ENOUGH MEMORY","SYNCING LOOP LENGTH","SELECT THIS AUDIO TRACK","PREPARING MPC TRACK","WAIT MPC TRACK BRIDGE","WAIT INPUT THRESHOLD","END CAPTURE TO REVERSE",
    "ARMED: TAKE START",
#ifdef RETROLOOPER_FORCE_BOUNCE
    "BOUNCED TO CLIP","SELECT AUDIO CLIP SLOT","CLIP CHANGED; WAV SAVED","STOP FORCE TO BOUNCE","CLIP FAILED; WAV SAVED","FORCE CONTROLS NEEDED","CLIP BOUNCE BUSY","END CAPTURE TO BOUNCE","PICK PAD OR MATRIX","BOUNCE CANCELED",
#else
    "BOUNCED TO TRACK","TRACK MOVED | WAV SAVED","FAILED | WAV SAVED","BUSY | WAV SAVED","STOP MPC | WAV SAVED","OVERLAP | WAV SAVED","END CAPTURE TO BOUNCE",
#endif
};
constexpr int lastMessage=sizeof(messages)/sizeof(messages[0])-1;

static float norm(int i,float value) {return std::clamp((value-parameters[i].low)/(parameters[i].high-parameters[i].low),0.f,1.f);}
static bool retired(int i){return i==tempo||i==monitor||i==host_sync||i==click||i==click_gain||i==crossfader||i==assign;}
static void text(void* p,const char* s,int size) {if(p)std::snprintf(static_cast<char*>(p),size,"%s",s);}
static Audio resample(Audio a,double rate) {
    if(std::abs(a.rate-rate)<.5)return a;
    const auto n=static_cast<std::size_t>(std::llround(a.left.size()*rate/a.rate));
    Audio b;b.rate=rate;b.left.resize(n);b.right.resize(n);
    const double ratio=a.rate/rate,cutoff=std::min(1.,1./ratio)*.95;
    for(std::size_t i=0;i<n;++i) {
        const double pos=i*ratio;const auto center=static_cast<int64_t>(pos);double l=0,r=0,sum=0;
        for(int t=-16;t<=16;++t) {const auto index=std::clamp<int64_t>(center+t,0,a.left.size()-1);const double x=pos-(center+t);
            if(std::abs(x)>16)continue;const double z=3.141592653589793*x*cutoff;
            const double w=(std::abs(z)<1e-12?1:std::sin(z)/z)*(.5+.5*std::cos(3.141592653589793*x/16))*cutoff;
            l+=a.left[index]*w;r+=a.right[index]*w;sum+=w;}
        b.left[i]=static_cast<float>(l/sum);b.right[i]=static_cast<float>(r/sum);
    }
    return b;
}

struct Plugin {
    AEffect fx{};HostCallback host=nullptr;
    using RecCounter=uint32_t(*)();RecCounter physicalRec=nullptr,physicalReady=nullptr;uint32_t lastPhysicalRec=0;
    using TrackStatus=uint32_t(*)(void*);using TrackPrepare=void(*)(void*);
    TrackStatus nativeTrackStatus=nullptr;TrackPrepare nativeTrackPrepare=nullptr;
    using PadTake=int(*)(void*,uint32_t,uint32_t*);
    using PadPublish=uint32_t(*)(void*,uint32_t,uint32_t,const uint8_t*);
    PadTake nativePadTake=nullptr;PadPublish nativePadPublish=nullptr;
#ifdef RETROLOOPER_FORCE_BOUNCE
    int(*nativeClipTarget)(void*,RloForceClipTarget*)=nullptr;
    int(*nativeClipEnqueue)(void*,const RloForceClipTarget*,const char*,uint32_t*)=nullptr;
    int(*nativeClipResult)(uint32_t,int)=nullptr;
    int(*nativePickerBegin)(void*,uint32_t)=nullptr;
    int(*nativePickerPoll)(void*,uint32_t,RloForceClipTarget*,int)=nullptr;
    std::atomic<uint32_t> pickerProject{0},pickerTrack{0};
    std::atomic<uint32_t> pickerGeneration{0};
    std::atomic<bool> cancelNativePicker{false};
#else
    int(*nativeTimelineTarget)(void*,RloMpcTimelineTarget*)=nullptr;
    int(*nativeTimelineEnqueue)(void*,const RloMpcTimelineTarget*,const char*,uint32_t*)=nullptr;
    int(*nativeTimelineResult)(uint32_t,int)=nullptr;
#endif
    uint32_t padToken=nextPadToken.fetch_add(1);std::atomic<uint32_t> padFlags{0};
#ifdef RETROLOOPER_TESTING
    bool testTrackGuard=false;
#endif
    bool lastTrackAllowed=false;int pendingCaptureAction=-1;
    int manualDub=-1; // -1 follows MPC; touch/MIDI overrides until its next transport/record edge.
    bool loopSuppressed=false,localDubPending=false;

    std::unique_ptr<LooperEngine> engine=std::make_unique<LooperEngine>();
    std::array<std::atomic<float>,param_count> values{};
    std::array<std::atomic<int64_t>,param_count> hostPressMs{};
    std::atomic<int64_t> lastControlMs{0};
    std::atomic<bool> protectPrepareRecord{false};
    std::atomic<float> prepareRecordValue{0};
    std::array<std::atomic<int>,3*midiTargetCount> notes{};
    std::array<std::atomic<int>,midiTargetCount> ccs{};
    BoundedCommandQueue<int,256> actions;
    BoundedCommandQueue<Job,256> jobs;
    std::atomic<uint32_t> generation{0};
    BoundedCommandQueue<Event,512> midi;
    std::atomic<bool> quit{false},active{false},editing{false};
    std::atomic<int> activeJob{-1};
    std::atomic<int> messageCode{Ready},stateCode{0},beat{1};
    std::atomic<float> seconds{0},positionValue{0},inputPeak{0},outputPeak{0},bpm{120};
    std::atomic<bool> undoReady{false},redoReady{false};
    std::mutex gate,stateGate;
    std::thread worker;
    std::vector<uint8_t> chunk;
    std::array<std::array<float,kMaxBlock>,2> inputScratch{},outputScratch{};
    PluginMidiGestures gestures;
    std::array<bool,128> ccDown{};
    std::array<float,4> visualPeaks{},visualSquares{},meterPeaks{},meterRms{};
    std::array<float,param_count> lastReported{};
    int visualSamples=0,visualRefreshFrames=0,lastReportedView=-1,lastVisualState=-1;
    meter_ui::TrackMeterBallistics focusBallistics;
    CassetteTransport cassette;
    LoopFilters filters;
    double rate=44100,lastManualTempo=-1,expectedBeat=0;
    bool prepared=false,hadHost=false;
    std::atomic<bool> hostPlaying{false},hostRecording{false},hostValid{false},hostCycleValid{false};
    bool hostRecordArmed=false,hostCapture=false,launchPending=false;
    bool manualRecordPending=false;
    std::atomic<std::size_t> storageRequested{0},storagePrepared{0},failedStorageRequest{0};
    std::atomic<std::size_t> loopLengthRequested{0},loopLengthApplied{0},conformFailed{0};
    std::size_t sourceFrames=0;
    bool sourceIncomplete=false;
    std::atomic<uint32_t> loopRevision{0};
    std::unique_ptr<LoopFallback> fallbackAudio;
    std::atomic<bool> fallbackActive{false};
    std::atomic<unsigned> fallbackReaders{0};
    std::atomic<int64_t> fallbackFrames{0};
    std::atomic<uint32_t> seekCount{0},droppedBlocks{0};
    std::atomic<float> peakClockError{0};
#ifdef RETROLOOPER_TESTING
    std::atomic<uint64_t> testFallbackTotal{0};
#endif
    int64_t captureRemaining=0,captureTotal=0;
    double hostBeat=0,hostTempo=120,hostCycleStart=0,hostCycleEnd=0;
    std::atomic<float> sequenceBars{0},captureBars{0};
    int64_t continuousSamples=0;int displayFrames=0,animationFrames=0,lastUiState=-1,lastUiMessage=-1;
    std::filesystem::path loopFolder,exportFolder;

    explicit Plugin(HostCallback callback):host(callback) {
        for(int i=0;i<param_count;++i)values[i].store(norm(i,parameters[i].initial));
        lastReported.fill(-1);
        for(auto& note:notes)note.store(-1);
        for(auto& cc:ccs)cc.store(-1);
        for(int i=0;i<7;++i){notes[i].store(36+i);ccs[i].store(20+i);}
        Dl_info info{};
        // MPC OS 3.10.1 loads the folder from any drive's "VST2 Plugins".
        // Loops and exports stay beside the .so, never on a fixed /sdcard path.
        if(const char* dir=std::getenv("RETROLOOPER_TEST_LOOPS")){
            loopFolder=dir;
            exportFolder=loopFolder;
        }else if(dladdr(reinterpret_cast<void*>(&norm),&info)&&info.dli_fname){
            const auto pluginDir=std::filesystem::path(info.dli_fname).parent_path();
            loopFolder=pluginDir/"Loops";
            exportFolder=pluginDir/"Exports";
        }else{
            loopFolder="Loops";
            exportFolder="Exports";
        }
#ifdef RETROLOOPER_TESTING
        if(!std::getenv("RETROLOOPER_TEST_NO_CONTROLS")){
#endif
        physicalRec=reinterpret_cast<RecCounter>(dlsym(RTLD_DEFAULT,"retrolooper_mpc_rec_generation"));
        physicalReady=reinterpret_cast<RecCounter>(dlsym(RTLD_DEFAULT,"retrolooper_mpc_button_ready"));
        if(physicalRec)lastPhysicalRec=physicalRec();
        nativeTrackStatus=reinterpret_cast<TrackStatus>(dlsym(RTLD_DEFAULT,"retrolooper_mpc_track_status"));
        nativeTrackPrepare=reinterpret_cast<TrackPrepare>(dlsym(RTLD_DEFAULT,"retrolooper_mpc_track_prepare"));
        nativePadTake=reinterpret_cast<PadTake>(dlsym(RTLD_DEFAULT,"retrolooper_mpc_pad_take"));
        nativePadPublish=reinterpret_cast<PadPublish>(dlsym(RTLD_DEFAULT,"retrolooper_mpc_pad_publish"));
#ifdef RETROLOOPER_FORCE_BOUNCE
        nativeClipTarget=reinterpret_cast<decltype(nativeClipTarget)>(dlsym(RTLD_DEFAULT,"retrolooper_force_clip_target"));
        nativeClipEnqueue=reinterpret_cast<decltype(nativeClipEnqueue)>(dlsym(RTLD_DEFAULT,"retrolooper_force_clip_enqueue"));
        nativeClipResult=reinterpret_cast<decltype(nativeClipResult)>(dlsym(RTLD_DEFAULT,"retrolooper_force_clip_result"));
        nativePickerBegin=reinterpret_cast<decltype(nativePickerBegin)>(dlsym(RTLD_DEFAULT,"retrolooper_force_picker_begin"));
        nativePickerPoll=reinterpret_cast<decltype(nativePickerPoll)>(dlsym(RTLD_DEFAULT,"retrolooper_force_picker_poll"));
#else
        nativeTimelineTarget=reinterpret_cast<decltype(nativeTimelineTarget)>(dlsym(RTLD_DEFAULT,"retrolooper_mpc_timeline_target"));
        nativeTimelineEnqueue=reinterpret_cast<decltype(nativeTimelineEnqueue)>(dlsym(RTLD_DEFAULT,"retrolooper_mpc_timeline_enqueue"));
        nativeTimelineResult=reinterpret_cast<decltype(nativeTimelineResult)>(dlsym(RTLD_DEFAULT,"retrolooper_mpc_timeline_result"));
#endif
#ifdef RETROLOOPER_TESTING
        }
        testTrackGuard=std::getenv("RETROLOOPER_TEST_TRACK_GUARD")!=nullptr;
#endif
        worker=std::thread([this]{work();});
    }
    ~Plugin(){
        quit.store(true);if(worker.joinable())worker.join();
#ifdef RETROLOOPER_FORCE_BOUNCE
        if(nativePickerPoll)nativePickerPoll(&fx,padToken,nullptr,1);
#endif
    }
    float value(int i) const {return parameters[i].low+values[i].load(std::memory_order_relaxed)*(parameters[i].high-parameters[i].low);}
    int choice(int i) const {return static_cast<int>(std::lround(value(i)));}
    int learnTarget() const {return midiVisibleTargets[std::clamp(choice(learn_target),0,midiVisibleCount-1)];}
    bool on(int i) const {return values[i].load(std::memory_order_relaxed)>=.5f;}
    void setFromHost(int i,float n) {
        if(i<0||i>=param_count||!std::isfinite(n))return;
        if(i>=pad_a_01&&i<=pad_a_16){
            // Choice knobs use a Schmitt boundary: touch jitter near the half
            // step cannot flicker between two functions. Exact Q-Link steps,
            // automation choices, MIDI Learn and saved assignments stay exact.
            const float range=parameters[i].high-parameters[i].low;
            const float old=value(i),requested=parameters[i].low+std::clamp(n,0.f,1.f)*range;
            if(std::abs(requested-old)<.65f)n=norm(i,old);
        }
        // JUCE/MPC toggles parameter 0 high then back during prepare and STOP.
        // Hold only this preparation window; do not interpret its net-zero probe
        // as pressing Record. Normal touch pulses and MIDI stay unchanged.
        if(i==record&&protectPrepareRecord.load()){prepareRecordValue.store(n);return;}
        // MPC can report finger-down and finger-up as two highs 30..140 ms apart.
        // Spring-back of values[] is visual state, not a new physical press.
        // Match the pinned community wrapper's 250 ms per-trigger guard.
        // MIDI calls trigger()/set() directly and retains sample-offset timing.
        if(parameters[i].kind==2&&n>=.5f) {
            const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            auto previous=hostPressMs[i].load(std::memory_order_relaxed);
            do {if(previous&&now-previous<250)return;}
            while(!hostPressMs[i].compare_exchange_weak(previous,now,std::memory_order_relaxed));
        }
        if(parameters[i].kind<3&&std::abs(values[i].load()-n)>.00001f)
            lastControlMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        set(i,n);
    }
    void set(int i,float n) {
        if(i<0||i>=param_count||!std::isfinite(n)||parameters[i].kind==3)return;
        n=std::clamp(n,0.f,1.f);
        if(parameters[i].kind==2) {
            const float before=values[i].exchange(n);
            if(n>=.5f&&before<.5f) {
                if(i<=sync_start||(i>=pad_press_01&&i<=pad_press_16)) {if(!actions.push(i))messageCode.store(QueueFull);}
                else if(i==forget){const int t=learnTarget();notes[choice(gesture)*midiTargetCount+t].store(-1);ccs[t].store(-1);}
                else if(i==target_previous||i==target_next)values[learn_target].store(norm(learn_target,(choice(learn_target)+(i==target_next?1:midiVisibleCount-1))%midiVisibleCount));
                else {
                    Job job{i,choice(file_slot),choice(bars),generation.load()};
#ifdef RETROLOOPER_FORCE_BOUNCE
                    if(i==bounce_cancel){pickerProject=0;cancelNativePicker.store(true);messageCode.store(ClipCanceled);return;}
                    if(i==bounce_clip||i==bounce_commit) {
                        if(!controlsSupported()||!nativeClipTarget||!nativeClipEnqueue||!nativeClipResult){messageCode.store(ClipSetup);return;}
                        if(i==bounce_clip&&nativePickerBegin&&nativePickerPoll){
                            if(pickerGeneration.load()){cancelNativePicker.store(true);return;}
                            if(seconds.load()<=0){messageCode.store(NoLoop);return;}
                            const int result=nativePickerBegin(&fx,padToken);
                            if(result==RLO_CLIP_PENDING){
                                pickerGeneration.store(job.generation+1);messageCode.store(ClipPickSlot);
                            }else messageCode.store(result==RLO_CLIP_BUSY?ClipBusy:TrackNotSelected);
                            return;
                        }
                        const int result=nativeClipTarget(&fx,&job.clipTarget);
                        if(result==RLO_CLIP_TRANSPORT_RUNNING){messageCode.store(ClipStopFirst);return;}
                        if(result==RLO_CLIP_UNAVAILABLE){messageCode.store(ClipSetup);return;}
                        if(i==bounce_clip){
                            if(!job.clipTarget.project){messageCode.store(TrackNotSelected);return;}
                            if(seconds.load()<=0){messageCode.store(NoLoop);return;}
                            pickerTrack=job.clipTarget.track;pickerProject=job.clipTarget.project;
                            messageCode.store(ClipPickSlot);return;
                        }
                        if(result!=RLO_CLIP_BOUNCED){messageCode.store(SelectClip);return;}
                        if(!pickerProject.load()||pickerProject!=job.clipTarget.project||pickerTrack!=job.clipTarget.track){
                            pickerProject=0;messageCode.store(TrackNotSelected);return;
                        }
                        job.index=bounce_clip;
                        pickerProject=0;
                    }
#else
                    if(i==export_audio&&controlsSupported()&&nativeTimelineTarget&&nativeTimelineEnqueue&&nativeTimelineResult){
                        RloMpcTimelineTarget target{};
                        if(nativeTimelineTarget(&fx,&target)==RLO_TIMELINE_BOUNCED)job.timelineTarget=target;
                    }
#endif
                    messageCode.store(Busy);if(!jobs.push(job))messageCode.store(QueueFull);
                }
            }
        } else {
            if(parameters[i].kind==1||parameters[i].options[0])n=norm(i,std::round(parameters[i].low+n*(parameters[i].high-parameters[i].low)));
            values[i].store(n);
        }
    }
    bool prepare() {
        if(rate!=44100&&rate!=48000) {messageCode.store(UnsupportedRate);return false;}
        const double previousRate=engine->transport().sampleRate();const auto previousActive=engine->tracks()[0].lengthSamples();
        const auto previousSource=sourceFrames;
        if(previousSource>previousActive)engine->hostSnapshotSpan(previousSource);
        engine->prepare(rate,false);engine->setSpilloverTailSeconds(0);engine->setRealtimeActive(true);
        if(previousRate>0&&previousSource>previousActive)engine->hostSnapshotSpan(static_cast<std::size_t>(std::llround(previousActive*rate/previousRate)));
        if(previousRate>0)sourceFrames=static_cast<std::size_t>(std::llround(previousSource*rate/previousRate));
        // Legacy free-running mode has no known take length. Its old maximum
        // is prepared here only when explicitly enabled before activation.
        if(!hostMode())engine->reserveCaptureStorage(static_cast<std::size_t>(rate*120)+kMaxBlock);
        storageRequested.store(0);failedStorageRequest=0;
        storagePrepared.store(engine->captureStorageSize());
        gestures.prepare(rate);filters.prepare(rate);prepared=true;hadHost=false;return true;
    }
    PlaybackStartMode timingFor(int p) const {
        int c=choice(p);if(c==0)c=choice(timing)+1;
        return c==1?PlaybackStartMode::Immediate:c==2?PlaybackStartMode::Bar:PlaybackStartMode::Loop;
    }
    void configure() {
        engine->setFreeMode(false);engine->setSyncMode(SyncMode::Free);
        const int t=choice(timing);const auto mode=t==0?PlaybackStartMode::Immediate:t==1?PlaybackStartMode::Bar:PlaybackStartMode::Loop;
        engine->setPlaybackStartMode(mode);engine->setRecordStartMode(mode);engine->setOverdubStartMode(mode);engine->setMuteStartMode(mode);
        engine->setGlobalStopMode(t==0?GlobalStopMode::Immediate:t==1?GlobalStopMode::Bar:GlobalStopMode::LoopEnd);
        TrackLoopPrefs prefs;prefs.overrideGlobal=true;prefs.playMode=static_cast<uint8_t>(timingFor(play_timing));
        prefs.recordMode=static_cast<uint8_t>(timingFor(rec_timing));prefs.overdubMode=static_cast<uint8_t>(timingFor(dub_timing));
        prefs.muteMode=static_cast<uint8_t>(timingFor(mute_timing));
        const auto stopMode=timingFor(stop_timing);prefs.stopMode=static_cast<uint8_t>(stopMode==PlaybackStartMode::Immediate?GlobalStopMode::Immediate:stopMode==PlaybackStartMode::Bar?GlobalStopMode::Bar:GlobalStopMode::LoopEnd);
        prefs.afterRecordMode=static_cast<uint8_t>(hostMode()&&hostRecordControl()&&wantsDub()&&trackAllowed()?1:choice(after_record));engine->setTrackLoopPrefs(0,prefs);
        engine->setAfterRecordMode(static_cast<AfterRecordMode>(prefs.afterRecordMode));
        engine->setLengthQuantizeGrid(!on(host_sync)&&t?LengthQuantizeGrid::Bar:LengthQuantizeGrid::Off);
        engine->setDubModeEnabled(on(record_mode));engine->setMuteModeEnabled(on(mute_mode));
        engine->setCountInStyle(CountInStyle::Off);engine->setCountOutStyle(CountOutStyle::Off);
        engine->setMasterGain(value(master));engine->setOverdubFeedback(value(feedback));engine->setLoopCrossfadeMilliseconds(value(crossfade_ms));
        engine->setCrossfaderPosition(hostMode()?.5f:value(crossfader));engine->setTrackCrossfadeAssign(0,hostMode()?CrossfadeAssign::Off:static_cast<CrossfadeAssign>(choice(assign)));
        filters.set(value(hp_cutoff),value(lp_cutoff));
        TrackMix mix=engine->tracks()[0].mix();mix.inputGain=1;mix.volume=value(volume);mix.pan=value(pan);mix.directMonitor=false;engine->setTrackMix(0,mix);
        // "Hardware" here means the buffers supplied to this VST insert, not an
        // ALSA device. The private engine's main bus returns only to out[0/1].
        TrackIoRouting io;io.recordInput=TrackRecordInput::Hardware;io.hardwareInputStartChannel=0;
        io.recordMode=io.playbackMode=TrackIoChannelMode::Stereo;
        io.hardwareOutputStartChannel=on(output)?TrackIoRouting::kSilentOutput:TrackIoRouting::kUseMainStereoBus;
        engine->setTrackIoRouting(0,io);engine->setMetronomeEnabled(!on(host_sync)&&on(click));engine->setMetronomeGain(value(click_gain));
        // Direct monitor is mixed here so its 0..4 input gain matches the desktop plug-in.
    }
    uint32_t trackStatus() const {return nativeTrackStatus&&nativeTrackPrepare?nativeTrackStatus(const_cast<AEffect*>(&fx)):0;}
    bool controlsSupported() const {return (trackStatus()&RLO_TRACK_SUPPORTED)!=0;}
    bool managedTrack(uint32_t flags) const {
#ifdef RETROLOOPER_TESTING
        if(!testTrackGuard)return false;
#endif
        return (flags&RLO_TRACK_SUPPORTED)!=0;
    }
    bool trackSelected() const {
        constexpr auto mask=RLO_TRACK_READY|RLO_TRACK_SELECTED|RLO_TRACK_MONITOR_ROUTED;
        const auto flags=trackStatus();return !managedTrack(flags)||((flags&mask)==mask);
    }
    bool trackAllowed() const {
        constexpr auto mask=RLO_TRACK_READY|RLO_TRACK_SELECTED|RLO_TRACK_DISARMED|RLO_TRACK_MONITOR_ROUTED|RLO_TRACK_INPUT_READY;
        const auto flags=trackStatus();return !managedTrack(flags)||((flags&mask)==mask);
    }
    bool hostRecordControl() const {return choice(capture_source)==0&&controlsSupported()&&physicalRec&&physicalReady&&physicalReady();}
    void requestTrackPrepare(){if(managedTrack(trackStatus()))nativeTrackPrepare(&fx);}
    void enforceTrackCapture(){
        const auto flags=trackStatus();
        if(!managedTrack(flags))return;
        const bool ready=(flags&(RLO_TRACK_READY|RLO_TRACK_MONITOR_ROUTED))==(RLO_TRACK_READY|RLO_TRACK_MONITOR_ROUTED);
        const bool selected=ready&&(flags&RLO_TRACK_SELECTED);
        const bool allowed=selected&&(flags&RLO_TRACK_DISARMED)&&(flags&RLO_TRACK_INPUT_READY);
        // A failed cue may leave a selection/preparation message behind after
        // the user fixes its cause. Preserve unrelated worker/action results.
        int previous=messageCode.load(),next=previous;
        if(previous==TrackNotSelected||previous==TrackArmPending||previous==TrackBridgeUnavailable){
            next=!ready?TrackBridgeUnavailable:!selected?TrackNotSelected:
                previous==TrackArmPending&&!allowed?TrackArmPending:Ready;
            if(next!=previous)messageCode.compare_exchange_strong(previous,next);
        }
        if(allowed)return;
        const auto state=engine->tracks()[0].state();
        if(state==TrackState::Recording){sourceIncomplete=true;loopRevision.fetch_add(1);}
        engine->hostEndCaptureForSelection(on(record_mode),hostPlaying.load());
        hostCapture=false;captureRemaining=0;
        // Unknown/stale UI acknowledgement blocks audio capture, but isn't
        // evidence that the user changed tracks. Keep an explicit first-take
        // cue through UI refresh/preparation, then revalidate before starting.
        if(ready&&!selected){hostRecordArmed=false;pendingCaptureAction=-1;manualDub=-1;localDubPending=false;}
    }
    void observePhysicalRecord() {
        if(!physicalRec||!controlsSupported())return;
        const auto current=physicalRec(),delta=current-lastPhysicalRec;lastPhysicalRec=current;
        if(!delta||!hostMode()||!hostRecordControl()||!trackAllowed())return;
        const auto& track=engine->tracks()[0];
        if(track.lengthSamples()==0&&track.state()!=TrackState::Recording&&(delta&1)){
            hostRecordArmed=!hostRecordArmed;if(hostRecordArmed){failedStorageRequest=0;loopSuppressed=false;manualDub=-1;}messageCode.store(hostRecordArmed?SequenceArmed:Ready);
        }
    }
    double lengthMultiplier() const {return choice(length_ratio)==0?.5:choice(length_ratio)==2?2.:1.;}
    double captureSpan() const {return hostCycleValid?hostCycleEnd-hostCycleStart:engine->quartersPerNotatedBar()*choice(bars);}
    double captureOrigin() const {return hostCycleValid?hostCycleStart:0.;}
    bool hostMode() const {return on(host_sync);}
    bool wantsDub() const {return manualDub<0?(hostRecordControl()&&hostRecording.load()):manualDub!=0;}
    void followHost(int frames) {
        const auto* time=host&&hostMode()?reinterpret_cast<VstTimeInfo*>(host(&fx,audioMasterGetTime,0,
            kVstPpqPosValid|kVstTempoValid|kVstTimeSigValid|kVstCyclePosValid,nullptr,0)):nullptr;
        hostValid=time&&(time->flags&kVstTempoValid)&&(time->flags&kVstPpqPosValid)&&std::isfinite(time->tempo)&&time->tempo>=40&&time->tempo<=240
            &&std::isfinite(time->ppqPos)&&std::abs(time->ppqPos)<1e9;
        hostCycleValid=hostValid&&(time->flags&kVstCyclePosValid)&&std::isfinite(time->cycleStartPos)&&std::isfinite(time->cycleEndPos)
            &&time->cycleEndPos>time->cycleStartPos&&time->cycleEndPos-time->cycleStartPos<=4096;
        if(hostValid) {
            const bool wasPlaying=hadHost&&hostPlaying,wasRecording=hadHost&&hostRecording;const double previousTempo=hostTempo;
            const bool cycleChanged=hadHost&&hostCycleValid&&(std::abs(hostCycleStart-time->cycleStartPos)>1.e-6||std::abs(hostCycleEnd-time->cycleEndPos)>1.e-6);
            hostTempo=time->tempo;hostBeat=time->ppqPos;
            if(hostCycleValid){hostCycleStart=time->cycleStartPos;hostCycleEnd=time->cycleEndPos;}
            engine->commitExternalTempo(hostTempo);
            if((time->flags&kVstTimeSigValid)&&time->timeSigNumerator>=1&&time->timeSigNumerator<=32&&
               (time->timeSigDenominator==1||time->timeSigDenominator==2||time->timeSigDenominator==4||time->timeSigDenominator==8||time->timeSigDenominator==16))
                engine->setTimeSignature(time->timeSigNumerator,time->timeSigDenominator);
            const bool playing=time->flags&kVstTransportPlaying;
            const bool edge=!hadHost||playing!=hostPlaying;
            const bool followRecord=hostRecordControl();
            if(followRecord&&(edge||((time->flags&kVstTransportRecording)!=0)!=wasRecording)){manualDub=-1;loopSuppressed=false;}
            else if(!followRecord&&edge&&wasPlaying&&!playing){manualDub=0;localDubPending=false;}
            double error=hostBeat-expectedBeat;
            // A normal cycle wrap is continuous musical time, including during a 2x take.
            if(hostCycleValid&&(time->flags&kVstTransportCycleActive)){
                const double length=hostCycleEnd-hostCycleStart;error-=std::round(error/length)*length;
            }
            // MPC PPQ jitters by about .001 beat; tempo changes can rebase it slightly.
            const bool seek=wasPlaying&&playing&&!edge&&(cycleChanged||std::abs(error)>(std::abs(previousTempo-hostTempo)>.01?.25:.03));
            if(wasPlaying&&playing&&!edge){peakClockError.store(std::max(peakClockError.load(),static_cast<float>(std::abs(error))));if(seek)seekCount.fetch_add(1);}
            const double sequencePhase=hostBeat-(hostCycleValid?hostCycleStart:0);
            if(edge||seek){
                gestures.reset();
                if((!playing||seek)&&hostCapture&&captureRemaining>0)sourceIncomplete=true;
                if(!playing||seek){hostCapture=false;captureRemaining=0;if(wasPlaying)hostRecordArmed=false;}
                launchPending=playing&&choice(launch_mode)==1&&engine->tracks()[0].lengthSamples()>0;
                if(seek||launchPending)engine->followExternalTransport(false,sequencePhase,true);
                engine->followExternalTransport(playing&&!launchPending,sequencePhase,true);
                if(playing&&loopSuppressed){EngineCommand stop{CommandType::Stop,0,true};stop.trackCount=1;stop.executeAtSample=engine->transport().samplePosition();engine->enqueueCommand(stop);}
                if(!playing)filters.reset();
            }
            // The engine's clock helper folds PPQ to one bar. Rewriting its
            // shared playback origin every callback repeats bar one of a fresh
            // multi-bar take and turns PPQ jitter into audible phase jumps.
            // Anchor on genuine transport changes; sample time runs between them.
            engine->applyExternalBeatClock(hostTempo,sequencePhase,engine->quartersPerNotatedBar(),edge||seek);
            expectedBeat=hostBeat+(playing?frames*hostTempo/(60*rate):0);
            hostPlaying=playing;hostRecording=(time->flags&kVstTransportRecording)!=0;hadHost=true;
            if(hostRecording!=wasRecording)loopRevision.fetch_add(1);
            if(followRecord&&playing&&hostRecording&&trackSelected()&&!trackAllowed()
               &&(engine->tracks()[0].lengthSamples()>0||hostRecordArmed))requestTrackPrepare();
            if(playing&&!launchPending&&trackAllowed()){
                const auto state=engine->tracks()[0].state();
                if(wantsDub()&&((followRecord&&hostRecording&&!wasRecording)||!wasPlaying||!lastTrackAllowed||seek||localDubPending)){
                    // Repositioning restarts the engine's playback state. An
                    // existing loop must resume capture at that new phase while
                    // MPC stays in REC; there may be no later recording edge.
                    // First audio is explicit: only the plugin's Record action
                    // arms an empty take. MPC's combined REC/OVERDUB flag controls
                    // later layers, so Play/Overdub cannot create a loop by itself.
                    if(engine->tracks()[0].lengthSamples()>0&&!trackStateIsOverdub(state)&&state!=TrackState::Recording){
                        EngineCommand c{CommandType::OverdubToggle,0};c.trackCount=1;
                        c.executeAtSample=localDubPending&&timingFor(dub_timing)!=PlaybackStartMode::Immediate?engine->targetSampleForTimingMode(timingFor(dub_timing)==PlaybackStartMode::Bar?TimingMode::Bar:TimingMode::LoopEnd,0,engine->transport().samplePosition()):engine->transport().samplePosition();
                        if(engine->enqueueCommand(c))localDubPending=false;
                    }
                } else if(followRecord&&!wantsDub()&&!hostRecording&&wasRecording){
                    if(!hostRecording&&wasRecording)hostRecordArmed=false;
                    if(hostCapture&& !hostRecording&&wasRecording)closeHostCapture();
                    else if(trackStateIsOverdub(state)){
                        EngineCommand c{CommandType::OverdubToggle,0};c.trackCount=1;c.executeAtSample=engine->transport().samplePosition();engine->enqueueCommand(c);
                    }
                }
            }
            lastTrackAllowed=trackAllowed();
            const double bars=hostCycleValid?(hostCycleEnd-hostCycleStart)/engine->quartersPerNotatedBar():0;
            sequenceBars.store(static_cast<float>(bars));captureBars.store(static_cast<float>(captureSpan()/engine->quartersPerNotatedBar()*lengthMultiplier()));
            const auto& liveTrack=engine->tracks()[0];
            if(liveTrack.lengthSamples()>0&&liveTrack.state()!=TrackState::Recording&&hostCycleValid){
                const double reference=liveTrack.referenceBpm()>1?liveTrack.referenceBpm():hostTempo;
                const double wanted=(hostCycleEnd-hostCycleStart)*lengthMultiplier()*60*rate/reference;
                if(wanted>=2&&wanted<=rate*120){
                    const auto target=static_cast<std::size_t>(std::llround(wanted));
                    if(loopLengthRequested.exchange(target)!=target)conformFailed.store(0);
                }
            }
            if(!hostCycleValid)loopLengthRequested.store(0);
            if(engine->tracks()[0].lengthSamples()==0&&engine->tracks()[0].state()!=TrackState::Recording){
                const double frames=captureSpan()*lengthMultiplier()*60*rate/hostTempo;
                if(frames>=2&&frames<=rate*120)storageRequested.store(static_cast<std::size_t>(std::llround(frames))+kMaxBlock);
            }
        } else {
            if(hostMode()&&(hadHost||trackStateAdvancesPlayhead(engine->tracks()[0].state())||engine->tracks()[0].state()==TrackState::Recording))
                engine->followExternalTransport(false,0,true);
            if(lastManualTempo!=value(tempo)||hadHost){engine->commitUserTempo(value(tempo));lastManualTempo=value(tempo);}
            if(hadHost)engine->setTimeSignature(4,4);hadHost=false;hostPlaying=false;hostRecording=false;hostCapture=false;captureRemaining=0;launchPending=false;
            sequenceBars.store(0);captureBars.store(0);
            loopLengthRequested.store(0);
        }
    }
    int nextCaptureFrame(int offset,int frames,float** in,int limit) const {
        if(!hostMode()||!hostRecordArmed||!hostValid||!hostPlaying||!trackAllowed())return frames;
        if(on(auto_start)){
            const int route=choice(input);const double gain=value(input_gain),level=std::pow(10.,value(start_threshold)/20.);
            if(route==3||gain<=0||!in)return frames;
            for(int sample=offset;sample<limit;++sample)for(int ch=0;ch<2;++ch){
                const int source=route==1?0:route==2?1:ch;
                const float v=in[source]?in[source][sample]:0;
                if(std::isfinite(v)&&std::abs(static_cast<double>(v))*gain>=level)return sample;
            }
            return frames;
        }
        const double span=captureSpan(),perSample=hostTempo/(60*rate);
        double phase=std::fmod(hostBeat+offset*perSample-captureOrigin(),span);if(phase<0)phase+=span;
        const double delta=phase<.002||span-phase<.002?0:span-phase;
        return std::min(frames,offset+static_cast<int>(std::llround(delta/perSample)));
    }
    int nextLaunchFrame(int offset,int frames) const {
        if(!launchPending||!hostValid||!hostPlaying)return frames;
        const double bar=engine->quartersPerNotatedBar(),perSample=hostTempo/(60*rate);
        double phase=std::fmod(hostBeat+offset*perSample-(hostCycleValid?hostCycleStart:0),bar);if(phase<0)phase+=bar;
        const double delta=phase<.002||bar-phase<.002?0:bar-phase;
        return std::min(frames,offset+static_cast<int>(std::llround(delta/perSample)));
    }
    void launchFromBeginning(){
        launchPending=false;engine->followExternalTransport(true,0,true);messageCode.store(Ready);
        if(hostRecording&&hostRecordControl()&&trackAllowed()){EngineCommand c{CommandType::OverdubToggle,0};c.trackCount=1;c.executeAtSample=engine->transport().samplePosition();engine->enqueueCommand(c);}
    }
    void beginHostCapture(int offset) {
        if(!trackAllowed())return;
        const double count=captureSpan()*lengthMultiplier()*60*rate/hostTempo;
        if(count<2||count>rate*120){hostRecordArmed=false;messageCode.store(SequenceTooLong);return;}
        const auto needed=static_cast<std::size_t>(std::llround(count))+kMaxBlock;
        if(engine->captureStorageCapacity()<needed||(on(auto_start)&&engine->captureStorageSize()<needed)){storageRequested.store(needed);messageCode.store(failedStorageRequest==needed?MemoryLimit:PreparingAudio);return;}
        if(on(auto_start)){
            const double quarters=captureSpan()*lengthMultiplier();
            double phase=std::fmod(hostBeat+offset*hostTempo/(60*rate)-captureOrigin(),quarters);if(phase<0)phase+=quarters;
            engine->hostBeginThresholdCapture(static_cast<std::size_t>(std::llround(count)),static_cast<std::size_t>(std::llround(phase*60*rate/hostTempo)));
        } else {
            EngineCommand c{CommandType::RecordToggle,0};c.trackCount=1;c.executeAtSample=engine->transport().samplePosition();
            if(!engine->enqueueCommand(c)){messageCode.store(QueueFull);return;}
        }
        loopRevision.fetch_add(1);
        sourceFrames=0;sourceIncomplete=false;
        hostRecordArmed=false;hostCapture=true;captureRemaining=captureTotal=std::llround(count);messageCode.store(Ready);
    }
    void closeHostCapture() {
        loopRevision.fetch_add(1);
        if(captureRemaining>0)sourceIncomplete=true;
        if(engine->hostHasThresholdCapture())engine->hostFinishThresholdCapture();
        else {EngineCommand c{CommandType::RecordToggle,0};c.trackCount=1;c.executeAtSample=engine->transport().samplePosition();
            if(!engine->enqueueCommand(c))messageCode.store(QueueFull);}
        hostCapture=false;captureRemaining=0;
    }
    void trigger(int index,int offset=0) {
        if(index<0||index>sync_start)return;
        const bool captureAction=index==record||index==dub||(index==play&&on(record_mode));
        if(captureAction&&managedTrack(trackStatus())){
            if(!trackSelected()){constexpr auto ready=RLO_TRACK_READY|RLO_TRACK_MONITOR_ROUTED;messageCode.store((trackStatus()&ready)==ready?TrackNotSelected:TrackBridgeUnavailable);return;}
            requestTrackPrepare();
            if(!trackAllowed()){pendingCaptureAction=index;messageCode.store(TrackArmPending);return;}
        }

        if(index==record||index==dub||index==undo||index==redo||index==clear)loopRevision.fetch_add(1);
        if(index==clear){storagePrepared.store(0);sourceFrames=0;sourceIncomplete=false;loopLengthRequested.store(0);conformFailed.store(0);}
        if((index==stop||index==stop_now)&&hostCapture&&captureRemaining>0)sourceIncomplete=true;
        if(index==clear||index==stop||index==stop_now){pendingCaptureAction=-1;manualRecordPending=false;storageRequested.store(0);launchPending=false;}
        if(!hostMode()&&index==record&&engine->tracks()[0].lengthSamples()==0&&engine->tracks()[0].state()!=TrackState::Recording){
            const auto needed=static_cast<std::size_t>(rate*120)+kMaxBlock;
            if(engine->captureStorageCapacity()<needed){
                manualRecordPending=!manualRecordPending;storageRequested.store(manualRecordPending?needed:0);
                if(manualRecordPending)failedStorageRequest=0;
                messageCode.store(manualRecordPending?PreparingAudio:Ready);return;
            }
        }
        if(hostMode()) {
            const auto& tr=engine->tracks()[0];
            if(index==play&&launchPending){launchPending=false;loopSuppressed=true;return;}
            if(index==clear||index==stop||index==stop_now){hostRecordArmed=false;hostCapture=false;captureRemaining=0;manualDub=0;localDubPending=false;loopSuppressed=true;}
            if(index==record&&tr.lengthSamples()==0&&tr.state()!=TrackState::Recording){
                hostRecordArmed=!hostRecordArmed;
                if(hostRecordArmed){failedStorageRequest=0;loopSuppressed=false;manualDub=-1;}
                messageCode.store(hostRecordArmed?(!hostValid?HostUnavailable:hostCycleValid?SequenceArmed:ManualTakeArmed):Ready);return;
            }
            if((index==record||index==play)&&hostCapture){manualDub=0;configure();closeHostCapture();return;}
            if(!hostRecordControl()&&(index==record||index==dub)&&tr.lengthSamples()>0&&hostValid&&!hostPlaying){
                localDubPending=!localDubPending;manualDub=localDubPending?1:0;loopSuppressed=false;messageCode.store(localDubPending?SequenceArmed:Ready);return;
            }
            if((index==record||index==play||index==dub)&&(!hostValid||!hostPlaying)){
                messageCode.store(hostValid?HostStopped:HostUnavailable);return;
            }
            if(index==dub&&tr.lengthSamples()==0){messageCode.store(NoLoop);return;}
            if((index==record||index==dub)&&tr.lengthSamples()>0){const auto pending=engine->trackDiagnostics(0);localDubPending=false;manualDub=(trackStateIsOverdub(tr.state())||tr.state()==TrackState::Playing_DubPending||(pending.hasQueuedAction&&pending.queuedAction==CommandType::OverdubToggle))?0:1;loopSuppressed=false;index=dub;
                if(tr.state()==TrackState::Stopped)engine->followExternalTransport(true,hostBeat+offset*hostTempo/(60*rate)-(hostCycleValid?hostCycleStart:0),true);}
            if(index==play&&trackStateAdvancesPlayhead(tr.state())){manualDub=0;localDubPending=false;loopSuppressed=true;}
            if(index==play&&tr.state()==TrackState::Stopped&&tr.lengthSamples()>0){
                loopSuppressed=false;
                if(choice(launch_mode)==1)launchPending=true;
                else engine->followExternalTransport(true,hostBeat+offset*hostTempo/(60*rate)-(hostCycleValid?hostCycleStart:0),true);
                return;
            }
        }
        EngineCommand c{commands[index],0,index==stop_now};c.trackCount=1;
        // Anchor immediate MIDI events to their sample offset; quantized actions use engine grids.
        int p=index==record?rec_timing:index==dub?dub_timing:index==mute?mute_timing:index==stop||index==stop_now?stop_timing:play_timing;
        const auto& tr=engine->tracks()[0];
        if(index==play) {
            if(on(record_mode))p=tr.lengthSamples()>0?dub_timing:rec_timing;
            else if(on(mute_mode)&&trackStateAdvancesPlayhead(tr.state()))p=mute_timing;
            else if(trackStateAdvancesPlayhead(tr.state())||tr.state()==TrackState::Recording)p=stop_timing;
        }
        if(index==record&&tr.lengthSamples()>0)p=dub_timing;
        if(index==undo||index==redo||index==clear||index==sync_start||index==stop_now||timingFor(p)==PlaybackStartMode::Immediate)c.executeAtSample=engine->transport().samplePosition()+offset;
        else if(offset>0) {
            const auto mode=timingFor(p)==PlaybackStartMode::Bar?TimingMode::Bar:TimingMode::LoopEnd;
            c.executeAtSample=engine->targetSampleForTimingMode(mode,0,engine->transport().samplePosition()+offset);
        }
        if(!engine->enqueueCommand(c))messageCode.store(QueueFull);
    }
    void mapped(int target,int offset,int ccValue=-1) {
        const int p=midiTargets[target];
        if(retired(p))return;
        if(parameters[p].kind==2) {
            if(p<=sync_start)trigger(p,offset);
            else {set(p,0);set(p,1);set(p,0);}
        } else if(ccValue>=0)set(p,ccValue/127.f);
        else if(parameters[p].options[0]||parameters[p].kind==1) {
            const int next=choice(p)>=parameters[p].high?static_cast<int>(parameters[p].low):choice(p)+1;
            set(p,norm(p,next));
        } else {const float n=values[p].load()+1.f/127;set(p,n>1.0001f?0:std::min(1.f,n));}
    }
    void fireNote(int note,PluginMidiGesture gestureKind,int64_t at) {
        for(int i=0;i<midiTargetCount;++i)if(notes[static_cast<int>(gestureKind)*midiTargetCount+i].load()==note)
            mapped(i,static_cast<int>(std::max<int64_t>(0,at-continuousSamples)));
    }
    void handleMidi(const Event& e) {
        const int command=e.type&0xf0,n=e.data&127,v=e.value&127;
        if(command!=0x80&&command!=0x90&&command!=0xb0)return;
        const bool down=command==0xb0?v>=64:command==0x90&&v>0;
        if(on(learn)&&(down||command==0xb0)) {
            const int target=learnTarget(),g=choice(gesture);
            // A learned control owns its CC, or note+gesture, within this instance.
            if(command==0xb0){for(auto& c:ccs)if(c.load()==n)c.store(-1);ccs[target].store(n);ccDown[n]=down;}
            else {for(int i=0;i<midiTargetCount;++i)if(notes[g*midiTargetCount+i].load()==n)notes[g*midiTargetCount+i].store(-1);notes[g*midiTargetCount+target].store(n);}
            values[learn].store(0);gestures.reset();messageCode.store(Learned);return;
        }
        if(command==0xb0) {
            const bool edge=down&&!ccDown[n];ccDown[n]=down;
            for(int i=0;i<midiTargetCount;++i)if(ccs[i].load()==n) {
                if(parameters[midiTargets[i]].kind!=2)mapped(i,e.offset,v);
                else if(edge)mapped(i,e.offset);
            }
            return;
        }
        int mask=0;for(int g=0;g<3;++g)for(int i=0;i<midiTargetCount;++i)if(notes[g*midiTargetCount+i].load()==n)mask|=1<<g;
        gestures.message(e.type&15,n,down,continuousSamples+e.offset,mask,[&](int note,PluginMidiGesture g,int64_t at){fireNote(note,g,at);});
    }
    void publish() {
        const auto msg=messageCode.load();
        if(hostRecordArmed&&(msg==Ready||msg==SequenceArmed||msg==ManualTakeArmed||msg==WaitingInput||msg==HostStopped||msg==HostUnavailable
           ||msg==SequenceUnavailable||msg==TrackBridgeUnavailable||msg==TrackArmPending)){
            messageCode.store(!hostValid?HostUnavailable:
                !trackSelected()?TrackBridgeUnavailable:!trackAllowed()?TrackArmPending:
                !hostPlaying?HostStopped:on(auto_start)?WaitingInput:hostCycleValid?SequenceArmed:ManualTakeArmed);
        }
        const auto& tr=engine->tracks()[0];stateCode.store(hostRecordArmed||launchPending?static_cast<int>(TrackState::Armed):static_cast<int>(tr.state()));
        const auto len=tr.state()==TrackState::Recording?tr.recordedSamples():tr.lengthSamples();
        loopLengthApplied.store(len==loopLengthRequested.load()?len:0);
        const double stretch=hostMode()&&hostValid&&tr.referenceBpm()>1?hostTempo/tr.referenceBpm():1;
        seconds.store(static_cast<float>(len/rate/(tr.state()==TrackState::Recording?1:stretch)));
        values[saved_active_span].store(norm(saved_active_span,static_cast<float>(len)));
        const auto elapsed=engine->transport().samplePosition()-engine->trackLoopPhaseOrigin(0);
        double sourcePosition=len?std::fmod(elapsed*stretch,static_cast<double>(len)):0;if(sourcePosition<0)sourcePosition+=len;
        positionValue.store(hostMode()&&tr.state()==TrackState::Recording&&captureTotal>0?std::clamp(1.f-static_cast<float>(captureRemaining)/captureTotal,0.f,1.f):len&&trackStateAdvancesPlayhead(tr.state())?static_cast<float>(sourcePosition/len):0);
        bpm.store(static_cast<float>(engine->transport().tempoBpm()));undoReady.store(tr.hasOverdubUndoSnapshot());redoReady.store(tr.hasOverdubRedoSnapshot());
        const bool blink=(continuousSamples/std::max<int64_t>(1,static_cast<int64_t>(rate*.25)))%2==0;
        const auto state=tr.state();const auto diagnostic=engine->trackDiagnostics(0);
        const bool queued=diagnostic.hasQueuedAction;
        if(localDubPending||(queued&&diagnostic.queuedAction==CommandType::OverdubToggle&&state==TrackState::Playing))stateCode.store(static_cast<int>(TrackState::Playing_DubPending));
        auto light=[&](int p,bool active,bool pending){values[p].store(pending?float(blink):float(active));};
        light(record_indicator,state==TrackState::Recording,hostRecordArmed||state==TrackState::Armed);
        light(play_indicator,state==TrackState::Recording||trackStateAdvancesPlayhead(state),launchPending||(queued&&(diagnostic.queuedAction==CommandType::Play||diagnostic.queuedAction==CommandType::PlayStopToggle)));
        light(dub_indicator,trackStateIsOverdub(state),localDubPending||state==TrackState::Playing_DubPending||(queued&&diagnostic.queuedAction==CommandType::OverdubToggle));
        light(mute_indicator,trackStateIsSilent(state),trackStateIsMutePending(state));
        light(stop_indicator,state==TrackState::Stopped||state==TrackState::Empty,queued&&diagnostic.queuedAction==CommandType::Stop);
        values[clear_indicator].store(len>0);
        for(int pad=0;pad<16;++pad){const int action=choice(pad_a_01+pad);const float light=padBrightness(action);values[pad_indicator_01+pad].store(light);values[pad_face_01+pad].store((action*2+light)/(midiVisibleCount*2+1.f));}
        for(unsigned b=0;b<std::size(padBadgeFunctions);++b){
            int first=0,count=0;
            for(int pad=0;pad<16;++pad){const int action=choice(pad_a_01+pad);
                if(action>0&&action<=midiVisibleCount&&midiTargets[midiVisibleTargets[action-1]]==padBadgeFunctions[b]){if(!first)first=pad+1;++count;}}
            values[pad_label_record+b].store((first+count*.001f)/17.f);
        }
        const double b=(engine->transport().samplePosition()-engine->audiblePhaseOrigin())/engine->transport().samplesPerBeat();
        const auto beatIndex=static_cast<int64_t>(std::floor(b*engine->timeSigDenominator()/4.));
        beat.store(1+static_cast<int>((beatIndex%engine->timeSigNumerator()+engine->timeSigNumerator())%engine->timeSigNumerator()));
        if(engine->consumeRecordLimitReached())messageCode.store(Limit);
    }
    static float meterPosition(float amplitude) noexcept {
        return amplitude>1.e-6f?std::clamp((20.f*std::log10(amplitude)+48.f)/48.f,0.f,1.f):0.f;
    }
    void publishVisuals() noexcept {
        std::array<float,256> lo{},hi{};
        engine->copyWaveformEnvelopeMinMax(0,lo.data(),hi.data(),256);
        // Fold the complete 256-bin envelope; requesting just 48 bins would crop the loop.
        for(int column=0;column<48;++column) {
            float low=0,high=0;
            for(int b=column*256/48;b<(column+1)*256/48;++b){low=std::min(low,lo[b]);high=std::max(high,hi[b]);}
            values[wave_lo_00+column].store(std::clamp(-low,0.f,1.f));
            values[wave_hi_00+column].store(std::clamp(high,0.f,1.f));
        }
        const auto s=engine->tracks()[0].state();
        lastVisualState=static_cast<int>(s);
        values[reel_phase].store(static_cast<float>(cassette.supplyPhase));
        values[takeup_phase].store(static_cast<float>(cassette.takeupPhase));
        values[tape_fill].store(static_cast<float>(cassette.fill));
        if(visualSamples>0) {
            const float dt=visualSamples/static_cast<float>(rate),release=std::exp(-dt/.30f),rmsRelease=std::exp(-dt/.40f);
            constexpr int peaks[]{input_peak_l,input_peak_r,output_peak_l,output_peak_r};
            constexpr int rms[]{input_rms_l,input_rms_r,output_rms_l,output_rms_r};
            for(int channel=0;channel<4;++channel) {
                const float peak=meterPosition(visualPeaks[channel]);
                const float mean=meterPosition(std::sqrt(visualSquares[channel]/visualSamples));
                meterPeaks[channel]=peak>=meterPeaks[channel]?peak:peak+(meterPeaks[channel]-peak)*release;
                meterRms[channel]=mean+(meterRms[channel]-mean)*rmsRelease;
                values[peaks[channel]].store(meterPeaks[channel]);values[rms[channel]].store(meterRms[channel]);
            }
            metering::TrackMeterRtSnapshot snap;
            snap.inPeakL=visualPeaks[0];snap.inPeakR=visualPeaks[1];snap.outPeakL=visualPeaks[2];snap.outPeakR=visualPeaks[3];
            snap.inRmsL=std::sqrt(visualSquares[0]/visualSamples);snap.inRmsR=std::sqrt(visualSquares[1]/visualSamples);
            snap.outRmsL=std::sqrt(visualSquares[2]/visualSamples);snap.outRmsR=std::sqrt(visualSquares[3]/visualSamples);
            focusBallistics.process(dt,snap,s);
            values[focus_meter].store(focusBallistics.displayLevel());values[focus_hold].store(focusBallistics.peakHold());values[focus_clip].store(focusBallistics.clipLatched()?1.f:0.f);
            visualPeaks.fill(0);visualSquares.fill(0);visualSamples=0;
        }
    }
    bool visibleVisual(int parameter,int selected) const noexcept {
        if(parameter>=wave_lo_00&&parameter<=wave_hi_47)return selected==0||selected==1;
        if(parameter==reel_phase||parameter==takeup_phase||parameter==tape_fill)return selected==1;
        if(parameter>=input_peak_l&&parameter<=output_rms_r)return true;
        if(parameter>=focus_meter&&parameter<=focus_clip)return selected==2;
        return false;
    }
    float normalized(int i) const {
        if(i<0||i>=param_count)return 0;
        if(i==clock)return std::clamp((beat.load()-1)/32.f+(bpm.load()-40)/6400.f,0.f,1.f);
        if(i==binding){const int target=learnTarget(),g=choice(gesture);return (notes[g*midiTargetCount+target].load()+1)/258.f+(ccs[target].load()+1)/33282.f;}
        if(i==sequence_info)return std::clamp(sequenceBars.load()/32768.f+captureBars.load()/65536.f,0.f,1.f);
        if(i==host_transport)return !hostValid?0:!hostPlaying?.333333f:hostRecording?1:.666667f;
        if(i==button_status)return !controlsSupported()?.5f:float(trackSelected());
        if(i==clock_health)return (seekCount.load()%1000)/1000.f+(droppedBlocks.load()%1000)/1000000.f;
        if(i==status)return std::clamp(stateCode.load()/10.f,0.f,1.f);
        if(i==duration)return std::clamp(seconds.load()/120.f,0.f,1.f);
        if(i==position)return positionValue.load();
        if(i==in_meter)return std::min(1.f,inputPeak.load());
        if(i==out_meter)return std::min(1.f,outputPeak.load());
        if(i==message)return messageCode.load()/static_cast<float>(lastMessage);
#ifdef RETROLOOPER_FORCE_BOUNCE
        if(i==clip_picker)return pickerProject.load()?1.f:0.f;
        if(i==clip_indicator){
            const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            return (pickerGeneration.load()||activeJob.load()==bounce_clip)&&ms/250%2==0?1.f:0.f;
        }
#endif
        if(i==pad_status)return padFlags.load()/15.f;
        return values[i].load();
    }
    void display(int i,void* dest) const {
        char buf[24]{};
        if(i<0||i>=param_count)return text(dest,"",24);
        const int s=stateCode.load();
        if(i==pad_status){const auto flags=padFlags.load();text(dest,!nativePadPublish||!controlsSupported()?"TOUCH PADS / MIDI":!on(pad_bank_enabled)?"PAD BANK A OFF":!trackSelected()?"SELECT AUDIO TRACK":!(flags&2)?"PRESS PAD BANK A":!(flags&4)?"PAD A / LED WAIT":"PAD BANK A READY",24);return;}
        if(i>=pad_label_record&&i<=pad_label_export_audio){const float encoded=values[i].load()*17;const int pad=static_cast<int>(std::lround(encoded));
            if(pad)std::snprintf(buf,sizeof buf,"A%d%s",pad,encoded-pad>.0015?"+":"");else std::snprintf(buf,sizeof buf,"-");text(dest,buf,24);return;}
        if(i==sequence_info){if(!hostMode())text(dest,"MANUAL LENGTH",24);else if(!hostValid)text(dest,"WAIT MPC LENGTH",24);else{
            if(hostCycleValid)std::snprintf(buf,sizeof buf,"SEQ %.2g > %.2g B",sequenceBars.load(),captureBars.load());
            else std::snprintf(buf,sizeof buf,"TAKE %.2g B",captureBars.load());text(dest,buf,24);
        }return;}
        if(i==host_transport){text(dest,!hostMode()?"FREE":!hostValid?"WAIT MPC":hostPlaying?(hostRecording?"MPC REC":"MPC PLAY"):"MPC STOP",24);return;}
        if(i==host_click){text(dest,"MPC CLICK",24);return;}
        if(i==host_monitor){text(dest,"CHANNEL",24);return;}
        if(i==capture_source&&!controlsSupported()){text(dest,"Touch/Pads",24);return;}
        if(i==button_status){text(dest,!controlsSupported()?"USE TRACK MONITOR":!trackSelected()?"SELECT AUDIO TRACK":hostRecordControl()?"REC BUTTON READY":"TOUCH + PAD READY",24);return;}
        if(i==clock_health){std::snprintf(buf,sizeof buf,"S%u D%u E%.3f",seekCount.load(),droppedBlocks.load(),peakClockError.load());text(dest,buf,24);return;}
        if(i==status){constexpr const char* states[]{"EMPTY","RECORDING","PLAYING","OVERDUB","STOPPED","ARMED","MUTED","MUTE QUEUED","MUTE QUEUED","UNMUTE QUEUED","DUB QUEUED"};text(dest,states[std::clamp(s,0,10)],24);return;}
        if(i==message){text(dest,messages[std::clamp(messageCode.load(),0,lastMessage)],24);return;}
#ifdef RETROLOOPER_FORCE_BOUNCE
        if(i==clip_picker){text(dest,pickerProject.load()?"PICK DESTINATION":"",24);return;}
#endif
        if(i==start_threshold)std::snprintf(buf,sizeof buf,"%.1f dB",value(i));
        else if(i==duration)std::snprintf(buf,sizeof buf,"%.2f SEC",seconds.load());
        else if(i==position)std::snprintf(buf,sizeof buf,"%03.0f %%",positionValue.load()*100);
        else if(i==clock)std::snprintf(buf,sizeof buf,"%s %.1f BPM | %d",hostMode()?(!hostValid?"WAIT":!hostPlaying?"STOP":hostRecording?"REC":"PLAY"):"FREE",bpm.load(),beat.load());
        else if(i==binding) {const int t=learnTarget(),g=choice(gesture);std::snprintf(buf,sizeof buf,"NOTE %d / CC %d",notes[g*midiTargetCount+t].load(),ccs[t].load());}
        else if(i==hp_cutoff||i==lp_cutoff){const auto v=value(i);if((i==hp_cutoff&&v<=.000001f)||(i==lp_cutoff&&v>=.999999f))text(buf,"OFF",sizeof buf);else {const double hz=LoopFilters::hz(v);if(hz<1000)std::snprintf(buf,sizeof buf,"%.0f Hz",hz);else std::snprintf(buf,sizeof buf,"%.1f kHz",hz/1000);}}
        else if(i==in_meter||i==out_meter){const float peak=i==in_meter?inputPeak.load():outputPeak.load();std::snprintf(buf,sizeof buf,"%.1f dB",20*std::log10(std::max(.00001f,peak)));}
        else if(i==tempo)std::snprintf(buf,sizeof buf,"%.1f BPM",value(i));
        else if(i==feedback||i==click_gain)std::snprintf(buf,sizeof buf,"%.0f %%",value(i)*100);
        else if(i==crossfade_ms)std::snprintf(buf,sizeof buf,"%.1f ms",value(i));
        else if(i==pan){if(std::abs(value(i))<.005f)text(buf,"C",sizeof buf);else std::snprintf(buf,sizeof buf,"%.0f %s",std::abs(value(i))*100,value(i)<0?"L":"R");}
        else if(i==bars)std::snprintf(buf,sizeof buf,"%d bars",choice(i));
        else if(i==file_slot)std::snprintf(buf,sizeof buf,"Slot %02d",choice(i));
        else if(i==input_gain||i==volume||i==master)std::snprintf(buf,sizeof buf,"%.1f dB",20*std::log10(std::max(.00001f,value(i))));
        else if(parameters[i].options[0]) {
            const char* start=parameters[i].options;for(int n=0;n<choice(i)&&*start;++n){while(*start&&*start!='|')++start;if(*start)++start;}
            int n=0;while(start[n]&&start[n]!='|'&&n<23){buf[n]=start[n];++n;}buf[n]=0;
        } else std::snprintf(buf,sizeof buf,parameters[i].kind==1?"%.0f":"%.2f",value(i));
        text(dest,buf,24);
    }
    void bypass(float** in,float** out,int frames) const {
        if(hostMode()){
            for(int c=0;c<2;++c)if(out&&out[c])for(int i=0;i<frames;++i){const float v=in&&in[c]?in[c][i]:0;out[c][i]=std::isfinite(v)?v:0;}
            return;
        }
        const float gain=on(monitor)&&!on(output)?value(input_gain)*value(master)*value(volume):0;
        for(int c=0;c<2;++c)if(out&&out[c])for(int i=0;i<frames;++i) {
            const int source=choice(input)==1?0:choice(input)==2?1:c;
            out[c][i]=choice(input)==3?0:(in&&in[source]?in[source][i]*gain:0);
        }
    }
    bool renderFallback(float** in,float** out,int frames,bool replacing) noexcept {
        fallbackReaders.fetch_add(1);
        if(!fallbackActive.load()){fallbackReaders.fetch_sub(1);return false;}
        const auto& cache=*fallbackAudio;
        const auto offset=fallbackFrames.fetch_add(frames);
#ifdef RETROLOOPER_TESTING
        testFallbackTotal.fetch_add(frames);
#endif
        bool playing=!hostMode()||hostPlaying.load();double tempoNow=hostMode()?hostTempo:bpm.load();
        if(host&&hostMode()){const auto* t=reinterpret_cast<VstTimeInfo*>(host(&fx,audioMasterGetTime,0,kVstTempoValid,nullptr,0));if(t){playing=(t->flags&kVstTransportPlaying)!=0;if(std::isfinite(t->tempo)&&t->tempo>=40&&t->tempo<=240)tempoNow=t->tempo;}}
        const double step=tempoNow/cache.reference;
        const float leftGain=value(volume)*value(master)*panGainL(value(pan)),rightGain=value(volume)*value(master)*panGainR(value(pan));
        const int fade=static_cast<int>(std::llround(value(crossfade_ms)*rate/1000));
        filters.set(value(hp_cutoff),value(lp_cutoff));
        for(int i=0;i<frames;++i){
            float l=0,r=0;
            if(playing&&cache.audible){const double p=cache.position+(offset+i)*step;l=cache.sample(0,p,fade)*leftGain;r=cache.sample(1,p,fade)*rightGain;filters.process(l,r);}
            float dryL=in&&in[0]?in[0][i]:0,dryR=in&&in[1]?in[1][i]:0;
            if(std::isfinite(dryL))l+=dryL;if(std::isfinite(dryR))r+=dryR;
            if(replacing){out[0][i]=l;out[1][i]=r;}else {out[0][i]+=l;out[1][i]+=r;}
        }
        fallbackReaders.fetch_sub(1);return true;
    }
    void pressPad(unsigned pad){
        if(pad>=16||!trackSelected())return;
        if(on(learn)){
            values[pad_a_01+pad].store(norm(pad_a_01+pad,choice(learn_target)+1));values[learn].store(0);messageCode.store(Learned);return;
        }
        const int action=choice(pad_a_01+pad);
        if(action>0&&action<=midiVisibleCount)mapped(midiVisibleTargets[action-1],0);
    }
    void pollPads(){
        if(!nativePadTake||!controlsSupported())return;
        for(int n=0;n<16;++n){uint32_t pad=0;if(!nativePadTake(&fx,padToken,&pad))break;if(on(pad_bank_enabled))pressPad(pad);}
    }
    float padBrightness(int action) const {
        if(action<=0||action>midiVisibleCount)return 0;
        const int parameter=midiTargets[midiVisibleTargets[action-1]];
        const auto state=engine->tracks()[0].state();const auto diag=engine->trackDiagnostics(0);
        const bool blink=(continuousSamples/std::max<int64_t>(1,static_cast<int64_t>(rate*.25)))%2==0;int mode=0;
        if(parameter==record)mode=hostRecordArmed||state==TrackState::Armed?2:state==TrackState::Recording?1:0;
        else if(parameter==play)mode=launchPending||(diag.hasQueuedAction&&(diag.queuedAction==CommandType::Play||diag.queuedAction==CommandType::PlayStopToggle))?2:state==TrackState::Recording||trackStateAdvancesPlayhead(state)?1:0;
        else if(parameter==dub)mode=localDubPending||state==TrackState::Playing_DubPending||(diag.hasQueuedAction&&diag.queuedAction==CommandType::OverdubToggle)?2:trackStateIsOverdub(state)?1:0;
        else if(parameter==mute)mode=trackStateIsMutePending(state)?2:trackStateIsSilent(state)?1:0;
        else if(parameter==stop||parameter==stop_now)mode=state==TrackState::Stopped||state==TrackState::Empty;
        else if(parameter==clear)mode=engine->tracks()[0].lengthSamples()>0||(state==TrackState::Recording&&engine->tracks()[0].recordedSamples()>0);
        else if(parameter==undo)mode=undoReady.load();
        else if(parameter==redo)mode=redoReady.load();
        else if(activeJob.load()==parameter)mode=2;
#ifdef RETROLOOPER_FORCE_BOUNCE
        else if(parameter==bounce_clip&&pickerGeneration.load())mode=2;
#endif
        else if(parameters[parameter].kind<2&&parameters[parameter].high==1)mode=on(parameter);
        return mode==2?float(blink):float(mode==1);
    }
    void reportPads(){
        if(!nativePadPublish||!controlsSupported())return;
        uint8_t rgb[48]{};uint32_t captureMask=0;
        for(int pad=0;pad<16;++pad){
            const int action=choice(pad_a_01+pad);if(action<=0||action>midiVisibleCount)continue;
            const int parameter=midiTargets[midiVisibleTargets[action-1]];
            if(parameter==record||parameter==dub||(parameter==play&&on(record_mode)))captureMask|=1u<<pad;
            const float brightness=values[pad_indicator_01+pad].load()>.5f?1.f:.16f;
            for(int c=0;c<3;++c)rgb[pad*3+c]=static_cast<uint8_t>(std::lround(padColours[action][c]*brightness*127/255));
        }
        const uint32_t enabled=on(pad_bank_enabled)?1u|(captureMask<<16):0;
        padFlags.store(nativePadPublish(&fx,padToken,enabled,rgb));
    }
    void process(float** in,float** out,int frames,bool replacing=true) noexcept {
        if(frames<=0||!out||!out[0]||!out[1])return;
        const ScopedNoDenormals noDenormals;
        std::unique_lock lock(gate,std::try_to_lock);
        if(!lock.owns_lock()&&!fallbackActive.load()){
            // Snapshot pinning holds the lock only briefly. A bounded retry
            // avoids dropping a whole audio block for that small metadata copy.
            const auto until=std::chrono::steady_clock::now()+std::chrono::microseconds(50);
            do {if(lock.try_lock()||fallbackActive.load())break;}while(std::chrono::steady_clock::now()<until);
        }
        if(!lock.owns_lock()){if(!renderFallback(in,out,frames,replacing)){if(active.load()&&hostPlaying.load()&&seconds.load()>0)droppedBlocks.fetch_add(1);if(replacing)bypass(in,out,frames);}return;}
        if(!active.load()||!prepared){if(replacing)bypass(in,out,frames);return;}
        try {
            const auto missed=fallbackFrames.exchange(0);if(missed){engine->hostAdvanceGap(static_cast<int>(missed));expectedBeat+=missed*hostTempo/(60*rate);}
            const auto& currentTrack=engine->tracks()[0];
            if(currentTrack.state()!=TrackState::Recording&&currentTrack.lengthSamples()>0)sourceFrames=std::max(sourceFrames,currentTrack.lengthSamples());
            if(protectPrepareRecord.exchange(false)){
                const float pending=prepareRecordValue.exchange(0);if(pending>=.5f)setFromHost(record,pending);
            }
            configure();enforceTrackCapture();observePhysicalRecord();followHost(frames);
            pollPads();
            if(pendingCaptureAction>=0&&trackAllowed()){const int deferred=pendingCaptureAction;pendingCaptureAction=-1;trigger(deferred);}
            if(manualRecordPending){
                if(hostMode())manualRecordPending=false;
                else if(engine->captureStorageCapacity()>=storageRequested.load()){
                    manualRecordPending=false;messageCode.store(Ready);trigger(record);
                }
            }
            int action;for(int n=0;n<256&&actions.pop(action);++n){if(action>=pad_press_01&&action<=pad_press_16)pressPad(action-pad_press_01);else trigger(action);}
            std::array<Event,512> events{};int count=0;Event event;
            while(count<512&&midi.pop(event)){event.offset=std::clamp(event.offset,0,frames-1);events[count++]=event;}
            // Stable insertion sort in bounded storage; no allocation on the audio thread.
            for(int i=1;i<count;++i){const auto current=events[i];int j=i;while(j>0&&events[j-1].offset>current.offset){events[j]=events[j-1];--j;}events[j]=current;}
            float inPeak=0,outPeak=0;int nextEvent=0;
            for(int offset=0;offset<frames;) {
                while(nextEvent<count&&events[nextEvent].offset==offset){auto e=events[nextEvent++];e.offset=0;handleMidi(e);}
                const int untilEvent=nextEvent<count?events[nextEvent].offset-offset:frames-offset;
                const int startAt=nextCaptureFrame(offset,frames,in,offset+std::min(kMaxBlock,untilEvent));
                if(startAt==offset&&hostRecordArmed)beginHostCapture(offset);
                const int launchAt=nextLaunchFrame(offset,frames);
                if(launchAt==offset&&launchPending)launchFromBeginning();
                int n=std::min(kMaxBlock,untilEvent);
                if(startAt>offset)n=std::min(n,startAt-offset);
                if(launchAt>offset)n=std::min(n,launchAt-offset);
                if(hostCapture)n=static_cast<int>(std::min<int64_t>(n,captureRemaining));
                gestures.advance(continuousSamples+n-1,[&](int note,PluginMidiGesture g,int64_t at){fireNote(note,g,at);});
                configure();const int inputChoice=choice(input);
                for(int c=0;c<2;++c)for(int i=0;i<n;++i){const int source=inputChoice==1?0:inputChoice==2?1:c;float v=inputChoice==3||!in||!in[source]?0:in[source][offset+i];if(!std::isfinite(v))v=0;v*=value(input_gain);inputScratch[c][i]=v;inPeak=std::max(inPeak,std::abs(v));}
                for(int c=0;c<2;++c)for(int i=0;i<n;++i){const float v=inputScratch[c][i];visualPeaks[c]=std::max(visualPeaks[c],std::abs(v));visualSquares[c]+=v*v;}
                const float* inputs[]{inputScratch[0].data(),inputScratch[1].data()};float* outputs[]{outputScratch[0].data(),outputScratch[1].data()};
                engine->processBlock(inputs,outputs,n,2,2);
                const float lGain=panGainL(value(pan)),rGain=panGainR(value(pan));
                for(int i=0;i<n;++i) {
                  float wetL=outputScratch[0][i],wetR=outputScratch[1][i];
                  if(hostMode()&&(!hostValid||!hostPlaying)){wetL=wetR=0;}else filters.process(wetL,wetR);
                  for(int c=0;c<2;++c){
                    float v=c?wetR:wetL;
                    // Native monitor routing controls live input. Preserve the
                    // supplied channel buffer, including native timeline audio.
                    if(hostMode()){float dry=in&&in[c]?in[c][offset+i]:0;if(std::isfinite(dry))v+=dry;}
                    else if(on(monitor)&&!on(output))v+=inputScratch[c][i]*value(volume)*value(master)*engine->trackCrossfadeGain(0)*(c?rGain:lGain);
                    if(!std::isfinite(v))v=0;outPeak=std::max(outPeak,std::abs(v));
                    visualPeaks[c+2]=std::max(visualPeaks[c+2],std::abs(v));visualSquares[c+2]+=v*v;
                    if(replacing)out[c][offset+i]=v;else out[c][offset+i]+=v;
                  }
                }
                const auto motionState=engine->tracks()[0].state();
                cassette.advance(n/rate,motionState==TrackState::Recording||trackStateAdvancesPlayhead(motionState));
                values[reel_phase].store(static_cast<float>(cassette.supplyPhase));
                values[takeup_phase].store(static_cast<float>(cassette.takeupPhase));
                values[tape_fill].store(static_cast<float>(cassette.fill));
                visualSamples+=n;continuousSamples+=n;offset+=n;
                if(hostCapture){captureRemaining-=n;if(captureRemaining==0)closeHostCapture();}
            }
            inputPeak.store(inPeak);outputPeak.store(outPeak);publish();
            visualRefreshFrames+=frames;
            if(visualRefreshFrames>=rate/10||lastVisualState!=stateCode.load()){publishVisuals();visualRefreshFrames=0;}
        } catch(...) {bypass(in,out,frames);messageCode.store(FileError);}
        lock.unlock();
        // Strips move at 10 Hz; full text refreshes stay at 5 Hz. No host calls under the engine lock.
        // Smooth animation uses lightweight parameter invalidation only. Text,
        // waveform and meters retain the slower cadence; no 30 Hz UI rebuild.
        animationFrames+=frames;
        if(host&&animationFrames>=rate/30){
            animationFrames=0;
            for(int i:{position,reel_phase,takeup_phase}){
                if(i!=position&&choice(view)!=1)continue;
                const float v=normalized(i);if(v!=lastReported[i]){host(&fx,audioMasterAutomate,i,0,nullptr,v);lastReported[i]=v;}
            }
        }
        displayFrames+=frames;
        if(host&&displayFrames>=rate/10) {
            displayFrames=0;
            reportPads();
            for(int i=0;i<param_count;++i)if(parameters[i].kind==2&&values[i].exchange(0)>=.5f)host(&fx,audioMasterAutomate,i,0,nullptr,0);
            const int selected=choice(view);const bool changedView=selected!=lastReportedView;
            for(int i=status;i<=out_meter;++i)if(i!=position)host(&fx,audioMasterAutomate,i,0,nullptr,normalized(i));
            for(int i=wave_lo_00;i<param_count;++i)if(visibleVisual(i,selected)&&i!=reel_phase&&i!=takeup_phase) {
                // Waveforms only report changed strip frames. Hidden views do no host UI work.
                const int steps=(i==reel_phase||i==takeup_phase)?47:i==tape_fill?32:i>=input_peak_l?48:32;
                const float v=std::round(normalized(i)*steps)/steps;
                if(changedView||v!=lastReported[i]){host(&fx,audioMasterAutomate,i,0,nullptr,v);lastReported[i]=v;}
            }
            for(int i=sequence_info;i<param_count;++i)if(parameters[i].kind==3){const float v=normalized(i);if(v!=lastReported[i]){host(&fx,audioMasterAutomate,i,0,nullptr,v);lastReported[i]=v;}}
            lastReportedView=selected;
            for(int i=0;i<param_count;++i)if(parameters[i].kind<2) {
                const float v=normalized(i);if(lastReported[i]!=v){host(&fx,audioMasterAutomate,i,0,nullptr,v);lastReported[i]=v;}
            }
            const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            const int uiState=stateCode.load(),uiMessage=messageCode.load();
            if((uiState!=lastUiState||uiMessage!=lastUiMessage)&&now-lastControlMs.load()>=200){
                host(&fx,audioMasterUpdateDisplay,0,0,nullptr,0);lastUiState=uiState;lastUiMessage=uiMessage;
            }
        }
    }
    bool stopped() const {const auto s=engine->tracks()[0].state();return (s==TrackState::Stopped||s==TrackState::Empty)&&!engine->trackDiagnostics(0).hasQueuedAction;}
    void reverseLoop(const Job& request,std::unique_lock<std::mutex>& lock){
        const auto state=engine->tracks()[0].state();
        if(state==TrackState::Recording||trackStateIsOverdub(state)||hostCapture||hostRecordArmed){messageCode.store(FinishCapture);return;}
        const auto frames=engine->tracks()[0].lengthSamples();
        if(frames<2){messageCode.store(NoLoop);return;}
        const auto revision=loopRevision.load();const bool running=trackStateAdvancesPlayhead(state);
        auto finish=[&]{if(lock.owns_lock())lock.unlock();fallbackActive.store(false);while(fallbackReaders.load())std::this_thread::yield();fallbackAudio.reset();};
        try{
            if(running){
                const auto capacity=engine->captureStorageCapacity();
                lock.unlock();
                const bool budget=captureMemoryAvailable(capacity,capacity+frames/10);
                auto cache=std::make_unique<LoopFallback>();cache->audio.rate=rate;
                lock.lock();
                if(!budget)throw std::bad_alloc();
                if(generation.load()!=request.generation||loopRevision.load()!=revision){messageCode.store(Busy);return;}
                if(!engine->beginStateSnapshot()){messageCode.store(Busy);return;}
                lock.unlock();bool copied=false;
                try{copied=engine->copyStateSnapshot(0,cache->audio.left,cache->audio.right);}catch(...){engine->endStateSnapshot();throw;}
                engine->endStateSnapshot();lock.lock();
                const auto current=engine->tracks()[0].state();
                if(!copied||generation.load()!=request.generation||loopRevision.load()!=revision||current==TrackState::Recording||trackStateIsOverdub(current)||hostRecordArmed){messageCode.store(Busy);return;}
                cache->frames=frames;cache->reference=engine->tracks()[0].referenceBpm();if(cache->reference<1)cache->reference=bpm.load();
                cache->position=(engine->transport().samplePosition()-engine->trackLoopPhaseOrigin(0))*bpm.load()/cache->reference;
                cache->audible=trackStateAdvancesPlayhead(current)&&!trackStateIsSilent(current);
                fallbackAudio=std::move(cache);fallbackActive.store(true);
#ifdef RETROLOOPER_TESTING
                std::this_thread::sleep_for(std::chrono::milliseconds(testConformDelay.load()));
#endif
            }
            engine->setRealtimeActive(false);
            const bool changed=engine->hostReverseAtCurrentPhase();engine->setRealtimeActive(true);
            if(changed)loopRevision.fetch_add(1);
            messageCode.store(changed?Edited:NoLoop);publish();publishVisuals();finish();
        }catch(...){engine->setRealtimeActive(true);finish();throw;}
    }
#ifdef RETROLOOPER_FORCE_BOUNCE
    void bounceToClip(const Job& request,Audio& audio) {
        static std::atomic<uint32_t> serial{0};
        const auto stamp=std::chrono::system_clock::now().time_since_epoch().count();
        const auto path=std::filesystem::absolute(exportFolder/("Bounce-"+std::to_string(stamp)+"-"+std::to_string(serial.fetch_add(1))+".wav"));
        writeWave(path,audio);
        audio={}; // Release the render before Force allocates its own clip audio.
        if(quit.load()||request.generation!=generation.load())return;
        uint32_t ticket=0;
        int result=nativeClipEnqueue(&fx,&request.clipTarget,path.c_str(),&ticket);
        if(result==RLO_CLIP_PENDING) {
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
            while(!quit.load()&&request.generation==generation.load()&&std::chrono::steady_clock::now()<deadline) {
                result=nativeClipResult(ticket,0);
                if(result!=RLO_CLIP_PENDING)break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if(result==RLO_CLIP_PENDING)result=nativeClipResult(ticket,1);
        }
        if(quit.load()||request.generation!=generation.load())return;
        switch(result) {
            case RLO_CLIP_BOUNCED:messageCode.store(ClipBounced);break;
            case RLO_CLIP_SELECTION_CHANGED:messageCode.store(ClipChanged);break;
            case RLO_CLIP_TRANSPORT_RUNNING:messageCode.store(ClipStopFirst);break;
            case RLO_CLIP_BUSY:messageCode.store(ClipBusy);break;
            default:messageCode.store(ClipFailed);break;
        }
    }
#else
    void bounceToTimeline(const Job& request,Audio& audio) {
        static std::atomic<uint32_t> serial{0};
        const auto stamp=std::chrono::system_clock::now().time_since_epoch().count();
        const auto path=std::filesystem::absolute(exportFolder/("Bounce-"+std::to_string(stamp)+"-"+std::to_string(serial.fetch_add(1))+".wav"));
        writeWave(path,audio);
        audio={};
        if(quit.load()||request.generation!=generation.load())return;
        uint32_t ticket=0;
        int result=nativeTimelineEnqueue(&fx,&request.timelineTarget,path.c_str(),&ticket);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(result==RLO_TIMELINE_PENDING&&!quit.load()&&request.generation==generation.load()&&std::chrono::steady_clock::now()<deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            result=nativeTimelineResult(ticket,0);
        }
        if(result==RLO_TIMELINE_PENDING)result=nativeTimelineResult(ticket,1);
        if(quit.load()||request.generation!=generation.load())return;
        switch(result){
            case RLO_TIMELINE_BOUNCED:messageCode.store(TimelineBounced);break;
            case RLO_TIMELINE_CHANGED:messageCode.store(TimelineChanged);break;
            case RLO_TIMELINE_BUSY:messageCode.store(TimelineBusy);break;
            case RLO_TIMELINE_RUNNING:messageCode.store(TimelineStop);break;
            case RLO_TIMELINE_OCCUPIED:messageCode.store(TimelineOccupied);break;
            default:messageCode.store(TimelineFailed);break;
        }
    }
#endif
    void doJob(const Job& request) {
        const int job=request.index;
        bool render=job==export_audio;
#ifdef RETROLOOPER_FORCE_BOUNCE
        render=render||job==bounce_clip;
#endif
        Audio audio;std::filesystem::path path;
        char filename[24];std::snprintf(filename,sizeof filename,"Loop-%02d.wav",request.slot);path=exportFolder/filename;
        if(job==import_audio&&!waveExists(path))path=loopFolder/filename;
        if(job==import_audio)audio=readWave(path);
        std::unique_lock allocationLock(allocationGate,std::defer_lock);if(job==reverse)allocationLock.lock();
        std::unique_lock snapshotLock(stateGate,std::defer_lock);if(render||job==reverse)snapshotLock.lock();
        std::unique_lock lock(gate);
        if(request.generation!=generation.load()){messageCode.store(Ready);return;}
        if(render) {
#ifdef RETROLOOPER_FORCE_BOUNCE
            if(job==bounce_clip) {
                const auto state=engine->tracks()[0].state();
                if(state==TrackState::Recording||trackStateIsOverdub(state)||hostCapture){messageCode.store(ClipEndCapture);return;}
            }
#else
            if(request.timelineTarget.arrangement) {
                const auto state=engine->tracks()[0].state();
                if(state==TrackState::Recording||trackStateIsOverdub(state)||hostCapture){messageCode.store(TimelineCapture);return;}
            }
#endif
            audio.rate=rate;const float hp=value(hp_cutoff),lp=value(lp_cutoff),gain=value(volume)*value(master),panValue=value(pan);
            if(!engine->beginStateSnapshot()){messageCode.store(Busy);return;}
            lock.unlock();bool copied=false;
            try {copied=engine->copyStateSnapshot(0,audio.left,audio.right);}catch(...){engine->endStateSnapshot();throw;}
            engine->endStateSnapshot();snapshotLock.unlock();
            if(!copied||audio.left.empty()){messageCode.store(NoLoop);return;}
            LoopFilters renderFilters;renderFilters.prepare(audio.rate);renderFilters.set(hp,lp);
            const auto warmup=std::min<std::size_t>(audio.left.size(),static_cast<std::size_t>(audio.rate*.5));
            for(std::size_t i=audio.left.size()-warmup;i<audio.left.size();++i){float l=audio.left[i],r=audio.right[i];renderFilters.process(l,r);}
            for(std::size_t i=0;i<audio.left.size();++i){renderFilters.process(audio.left[i],audio.right[i]);audio.left[i]*=gain*panGainL(panValue);audio.right[i]*=gain*panGainR(panValue);}
#ifdef RETROLOOPER_FORCE_BOUNCE
            if(job==bounce_clip){bounceToClip(request,audio);return;}
#else
            if(request.timelineTarget.arrangement){bounceToTimeline(request,audio);return;}
#endif
            std::lock_guard exportLock(exportGate);int slot=request.slot;bool available=false;
            for(int n=0;n<64;++n){slot=1+(request.slot-1+n)%64;std::snprintf(filename,sizeof filename,"Loop-%02d.wav",slot);path=exportFolder/filename;if(!waveExists(path)){available=true;break;}}
            if(!available){messageCode.store(SlotExists);return;}
            writeWave(path,audio);values[file_slot].store(norm(file_slot,slot));messageCode.store(Exported);return;
        }
        if(job==reverse){reverseLoop(request,lock);return;}
        if(!stopped()){messageCode.store(StopToEdit);return;}
        engine->setRealtimeActive(false);
        struct Restore {LooperEngine& e;~Restore(){e.setRealtimeActive(true);}} restoreActive{*engine};
        bool changed=false;
        if(job==import_audio) {
            audio=resample(std::move(audio),rate);const float* data[]{audio.left.data(),audio.right.data()};
            changed=!audio.left.empty()&&engine->importAudioFile(0,data,2,audio.left.size(),true);
            if(changed){sourceFrames=audio.left.size();sourceIncomplete=false;engine->setTrackReferenceBpm(0,engine->transport().tempoBpm());messageCode.store(Imported);}
        } else if(job==trim||job==restore)changed=engine->trimLoopByBars(0,job==trim?-1:1);
        else {
            const int current=std::max(1,engine->loopWholeBarCount(0));
            const int target=job==half?std::max(1,current/2):job==double_length?std::min(64,current*2):request.targetBars;
            if(engine->samplesPerNotatedBar()*static_cast<std::size_t>(target)>rate*120){messageCode.store(Limit);return;}
            changed=engine->rescaleLoopToBarCount(0,target);
        }
        if(job!=import_audio||!changed)messageCode.store(changed?Edited:NoLoop);
        storageRequested.store(0);storagePrepared.store(engine->captureStorageSize());
        publish();publishVisuals();
    }
    void conformLoopLength() {
        const auto wanted=loopLengthRequested.load();
        if(wanted<2||wanted==loopLengthApplied.load()||wanted==conformFailed.load())return;
        std::unique_lock allocationLock(allocationGate,std::try_to_lock);if(!allocationLock.owns_lock())return;
        std::unique_lock snapshotLock(stateGate,std::try_to_lock);if(!snapshotLock.owns_lock())return;
        std::unique_lock lock(gate,std::try_to_lock);if(!lock.owns_lock()||!prepared||!hostMode())return;
        const auto& tr=engine->tracks()[0];const auto before=tr.lengthSamples();
        if(before<2||tr.state()==TrackState::Recording||trackStateIsOverdub(tr.state())||hostCapture)return;
        if(before==wanted){loopLengthApplied.store(wanted);return;}
        sourceFrames=std::max(sourceFrames,before);
        const double reference=tr.referenceBpm()>1?tr.referenceBpm():hostTempo;
        const double unit=rate*60/reference*engine->quartersPerNotatedBar()*.5;
        const bool repeat=!sourceIncomplete&&std::abs(sourceFrames-std::round(sourceFrames/unit)*unit)<=2;
        const auto revision=loopRevision.load(),stateGeneration=generation.load();
        const bool running=hostPlaying.load()&&trackStateAdvancesPlayhead(tr.state());
        auto finish=[&]{if(lock.owns_lock())lock.unlock();fallbackActive.store(false);while(fallbackReaders.load())std::this_thread::yield();fallbackAudio.reset();};
        try {
            const auto budgetCapacity=engine->captureStorageCapacity(),budgetNeeded=std::max(wanted,sourceFrames)+kMaxBlock;
            if(!captureMemoryAvailable(budgetCapacity,std::max(budgetCapacity,budgetNeeded)+(running?sourceFrames/10:0)))throw std::bad_alloc();
            std::unique_ptr<LoopFallback> cache;
            if(running){
                cache=std::make_unique<LoopFallback>();cache->audio.rate=rate;
                engine->hostSnapshotSpan(sourceFrames);const bool pinned=engine->beginStateSnapshot();engine->hostSnapshotSpan(before);
                if(!pinned)return;
                lock.unlock();bool copied=false;
                try {copied=engine->copyStateSnapshot(0,cache->audio.left,cache->audio.right);}catch(...){engine->endStateSnapshot();throw;}
                engine->endStateSnapshot();lock.lock();
                if(!copied||generation.load()!=stateGeneration||loopRevision.load()!=revision||engine->tracks()[0].state()==TrackState::Recording||trackStateIsOverdub(engine->tracks()[0].state()))return;
                const auto& current=engine->tracks()[0];
                cache->frames=wanted;cache->reference=current.referenceBpm()>1?current.referenceBpm():hostTempo;cache->repeat=repeat;
                cache->position=(engine->transport().samplePosition()-engine->trackLoopPhaseOrigin(0))*hostTempo/cache->reference;
                cache->audible=!trackStateIsSilent(current.state());fallbackAudio=std::move(cache);fallbackActive.store(true);
#ifdef RETROLOOPER_TESTING
                std::this_thread::sleep_for(std::chrono::milliseconds(testConformDelay.load()));
#endif
            }
            const auto capacity=engine->captureStorageCapacity(),needed=std::max(wanted,sourceFrames)+kMaxBlock;
            if(!captureMemoryAvailable(capacity,std::max(capacity,needed)+(running?sourceFrames/10:0)))throw std::bad_alloc();
            messageCode.store(MatchingLength);engine->reserveCaptureStorage(needed);
            if(!engine->hostFitLoopFrames(wanted,sourceFrames,repeat))throw std::runtime_error("Loop length storage is not ready");
            sourceFrames=std::max(sourceFrames,wanted);sourceIncomplete=false;storagePrepared.store(engine->captureStorageSize());conformFailed.store(0);
            messageCode.store(Ready);publish();publishVisuals();finish();
        }catch(const std::bad_alloc&){conformFailed.store(wanted);messageCode.store(MemoryLimit);finish();}
        catch(const std::exception&e){conformFailed.store(wanted);messageCode.store(FileError);std::fprintf(stderr,"RetroLooper length sync: %s\n",e.what());finish();}
    }
#ifdef RETROLOOPER_FORCE_BOUNCE
    bool takeForcePick(Job& job) {
        const uint32_t armed=pickerGeneration.load();
        if(cancelNativePicker.exchange(false)||(armed&&armed!=generation.load()+1)){
            if(nativePickerPoll)nativePickerPoll(&fx,padToken,nullptr,1);
            if(pickerGeneration.exchange(0))messageCode.store(ClipCanceled);
            return false;
        }
        if(!armed)return false;
        const int result=nativePickerPoll(&fx,padToken,&job.clipTarget,0);
        if(result==RLO_CLIP_PENDING)return false;
        pickerGeneration.store(0);
        if(result!=RLO_CLIP_BOUNCED){messageCode.store(ClipCanceled);return false;}
        job.index=bounce_clip;
        job.generation=armed-1;
        return true;
    }
#endif
    void work() {
        while(!quit.load()) {
            conformLoopLength();
            // Only an empty instance grows in the background. Existing loops
            // never lose playback to a speculative sequence-length allocation.
            if(storageRequested.load()>storagePrepared.load()&&storageRequested.load()!=failedStorageRequest.load()){
                std::unique_lock allocationLock(allocationGate,std::try_to_lock);
                std::unique_lock lock(gate,std::try_to_lock);
                if(allocationLock.owns_lock()&&lock.owns_lock()){
                    const auto wanted=storageRequested.load();const auto& tr=engine->tracks()[0];
                    if((wanted>engine->captureStorageCapacity()||wanted>engine->captureStorageSize())&&wanted!=failedStorageRequest&&
                       tr.lengthSamples()==0&&tr.state()!=TrackState::Recording&&!hostCapture){
                        try {if(!captureMemoryAvailable(engine->captureStorageCapacity(),wanted))throw std::bad_alloc();
                            engine->reserveCaptureStorage(wanted);storagePrepared.store(engine->captureStorageSize());failedStorageRequest=0;
                            if(messageCode.load()==PreparingAudio)messageCode.store(hostRecordArmed?SequenceArmed:Ready);
                        } catch(const std::bad_alloc&) {failedStorageRequest=wanted;messageCode.store(MemoryLimit);}
                    }
                }
            }
            Job job;
            const bool queued=jobs.pop(job);
#ifdef RETROLOOPER_FORCE_BOUNCE
            const bool picked=!queued&&takeForcePick(job);
            if(queued||picked) {
                if(job.generation!=generation.load()){
                    if(picked)nativePickerPoll(&fx,padToken,nullptr,1);
                    continue;
                }
#else
            if(queued) {
                if(job.generation!=generation.load())continue;
#endif
                editing.store(true);activeJob.store(job.index);messageCode.store(Busy);
                try {doJob(job);} catch(const std::exception& e) {std::fprintf(stderr,"RetroLooper file/edit error: %s\n",e.what());messageCode.store(FileError);} catch(...) {messageCode.store(FileError);}
#ifdef RETROLOOPER_FORCE_BOUNCE
                if(picked)nativePickerPoll(&fx,padToken,nullptr,1);
#endif
                editing.store(false);activeJob.store(-1);
            } else std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    static uint32_t hash(const uint8_t* p,std::size_t n){uint32_t h=2166136261u;for(std::size_t i=0;i<n;++i)h=(h^p[i])*16777619u;return h;}
    template<class T> static void append(std::vector<uint8_t>& dst,const T& value){const auto* p=reinterpret_cast<const uint8_t*>(&value);dst.insert(dst.end(),p,p+sizeof(T));}
    intptr_t save(void* pointer) {
        if(!pointer)return 0;
        std::lock_guard saveLock(stateGate);std::array<float,param_count> params;std::array<int32_t,4*midiTargetCount> bindings;
        double savedRate=rate,reference=120;int32_t cycles=1,beats=0,muted=0;bool pinned=false;
        {
            std::lock_guard lock(gate);const auto& tr=engine->tracks()[0];
            for(int i=0;i<param_count;++i)params[i]=(parameters[i].kind==2||parameters[i].kind==3||i==learn)?norm(i,parameters[i].initial):values[i].load();
            for(int i=0;i<3*midiTargetCount;++i)bindings[i]=notes[i].load();for(int i=0;i<midiTargetCount;++i)bindings[3*midiTargetCount+i]=ccs[i].load();
            savedRate=rate;reference=tr.referenceBpm();cycles=tr.cycleCount();beats=tr.beatsPerCycle();muted=engine->trackMuted(0);
            const auto activeFrames=tr.lengthSamples();params[saved_active_span]=norm(saved_active_span,static_cast<float>(activeFrames));
            const auto fullFrames=hostMode()?std::max(activeFrames,sourceFrames):activeFrames;
            if(fullFrames>activeFrames)engine->hostSnapshotSpan(fullFrames);
            pinned=engine->beginStateSnapshot();if(fullFrames>activeFrames)engine->hostSnapshotSpan(activeFrames);
        }
        if(!pinned)return 0;
        struct Unpin {LooperEngine& e;~Unpin(){e.endStateSnapshot();}} unpin{*engine};
        std::vector<float> l,r;if(engine->stateSnapshotFrames(0)>=2&&!engine->copyStateSnapshot(0,l,r))return 0;
        std::vector<uint8_t> next;const auto frames=static_cast<uint32_t>(l.size());next.reserve(64+sizeof(params)+sizeof(bindings)+frames*8ull);
        append(next,uint32_t(0x314f4c52));append(next,uint32_t(kStateVersion));append(next,uint32_t(param_count));append(next,savedRate);
        append(next,frames);append(next,reference);append(next,cycles);append(next,beats);append(next,muted);
        for(float v:params)append(next,v);append(next,uint32_t(midiTargetCount));for(int32_t v:bindings)append(next,v);
        for(const auto& channel:{&l,&r})for(float v:*channel)append(next,v);
        append(next,hash(next.data(),next.size()));chunk.swap(next);*static_cast<void**>(pointer)=chunk.data();return static_cast<intptr_t>(chunk.size());
    }
    bool load(const void* data,intptr_t size) {
        if(!data||size<52||size>48000*120*8+8192)return false;
        const auto* raw=static_cast<const uint8_t*>(data);std::size_t at=0;
        auto read=[&]<class T>(T& value){if(at+sizeof(T)>static_cast<std::size_t>(size))throw std::runtime_error("Truncated chunk");std::memcpy(&value,raw+at,sizeof(T));at+=sizeof(T);};
        uint32_t checksum;std::memcpy(&checksum,raw+size-4,4);if(checksum!=hash(raw,size-4))return false;
        uint32_t magic,version,count,frames;double savedRate,reference;int32_t cycles,beats,muted;
        read(magic);read(version);read(count);read(savedRate);read(frames);read(reference);read(cycles);read(beats);read(muted);
        const bool currentLayout=count==param_count
#ifdef RETROLOOPER_FORCE_BOUNCE
            ||count==266||count==267
#endif
            ;
        if(magic!=0x314f4c52||((version!=1&&version!=kStateVersion)|| (version==1?count!=162&&count!=kLegacyParams:count!=167&&count!=169&&count!=175&&count!=176&&count!=179&&count!=180&&count!=181&&count!=183&&count!=216&&count!=265&&!currentLayout))||(savedRate!=44100&&savedRate!=48000)||frames>savedRate*120||!std::isfinite(reference)||reference<0||reference>1000||cycles<1||cycles>64||beats<0||beats>4096||(muted!=0&&muted!=1))return false;
        std::array<float,param_count> params;std::array<int32_t,4*midiTargetCount> bindings;
        for(int i=0;i<param_count;++i)params[i]=norm(i,parameters[i].initial);
        for(uint32_t i=0;i<count;++i){read(params[i]);if(!std::isfinite(params[i])||params[i]<0||params[i]>1)return false;}
        if(count<=216)params[capture_source]=1; // Existing projects adopt explicit touch/pad capture.
        bindings.fill(-1);
        uint32_t targets=7;if(version>=2)read(targets);
        if(targets<7||targets>midiTargetCount)return false;
#ifdef RETROLOOPER_FORCE_BOUNCE
        if(targets<midiTargetCount)for(int i=pad_a_01;i<=pad_a_16&&i<static_cast<int>(count);++i)
            params[i]=norm(i,std::lround(params[i]*(targets-7)));
#endif
        for(int g=0;g<4;++g)for(uint32_t i=0;i<targets;++i){int32_t v;read(v);if(v<-1||v>127)return false;bindings[g*midiTargetCount+i]=v;}
        if(count<=176){
            const int oldTarget=static_cast<int>(std::lround(params[learn_target]*(targets-1)));
            int visible=0;for(int n=0;n<midiVisibleCount;++n)if(midiVisibleTargets[n]==oldTarget){visible=n;break;}
            params[learn_target]=norm(learn_target,visible);
        } else if(count<param_count){
            const int oldVisible=static_cast<int>(targets)-7;
            const int selected=std::clamp(static_cast<int>(std::lround(params[learn_target]*(oldVisible-1))),0,std::max(0,oldVisible-1));
            params[learn_target]=norm(learn_target,selected);
        }
        if(at+frames*8ull+4!=static_cast<std::size_t>(size))return false;
        Audio audio;audio.rate=savedRate;audio.left.resize(frames);audio.right.resize(frames);
        for(auto* channel:{&audio.left,&audio.right})for(float& v:*channel){read(v);if(!std::isfinite(v))return false;}
        audio=resample(std::move(audio),rate);
        std::lock_guard saveLock(stateGate);std::lock_guard lock(gate);engine->setRealtimeActive(false);
        struct Restore {Plugin& p;~Restore(){p.engine->setRealtimeActive(true);}} restoreActive{*this};
        auto restored=std::make_unique<LooperEngine>();restored->prepare(rate);
        restored->commitUserTempo(parameters[tempo].low+params[tempo]*(parameters[tempo].high-parameters[tempo].low));
        if(!audio.left.empty()){const float* channels[]{audio.left.data(),audio.right.data()};if(!restored->importAudioFile(0,channels,2,audio.left.size(),true))return false;}
        const auto fullFrames=audio.left.size();
        if(count>saved_active_span&&params[saved_active_span]>0){
            const auto savedActive=static_cast<std::size_t>(std::llround(params[saved_active_span]*8388608.));if(savedActive<2||savedActive>frames)return false;
            const auto activeFrames=static_cast<std::size_t>(std::llround(savedActive*rate/savedRate));if(activeFrames<2||activeFrames>fullFrames)return false;restored->hostSnapshotSpan(activeFrames);
        }
        restored->restoreTrackCycleMetadata(0,cycles,beats);restored->setTrackReferenceBpm(0,reference);restored->setTrackMuted(0,muted!=0);
        // Imported loops are stopped. Never auto-launch when opening a project.
        restored->setRealtimeActive(true);engine.swap(restored);
        for(int i=0;i<param_count;++i)values[i].store((parameters[i].kind>=2||i==learn)?norm(i,parameters[i].initial):params[i]);
        for(int i=0;i<3*midiTargetCount;++i)notes[i].store(bindings[i]);for(int i=0;i<midiTargetCount;++i)ccs[i].store(bindings[3*midiTargetCount+i]);
        generation.fetch_add(1);sourceFrames=fullFrames;sourceIncomplete=false;loopRevision.fetch_add(1);loopLengthRequested.store(0);loopLengthApplied.store(0);conformFailed.store(0);storageRequested.store(0);storagePrepared.store(engine->captureStorageSize());failedStorageRequest=0;manualRecordPending=false;int discard;while(actions.pop(discard)){}Event e;while(midi.pop(e)){}
        gestures.reset();filters.prepare(rate);ccDown.fill(false);for(auto& stamp:hostPressMs)stamp.store(0);lastManualTempo=-1;hadHost=false;hostRecordArmed=false;hostCapture=false;launchPending=false;manualDub=-1;localDubPending=false;loopSuppressed=false;captureRemaining=0;captureTotal=0;animationFrames=0;if(physicalRec)lastPhysicalRec=physicalRec();cassette=CassetteTransport{};visualSamples=0;visualRefreshFrames=0;
        visualPeaks.fill(0);visualSquares.fill(0);meterPeaks.fill(0);meterRms.fill(0);focusBallistics.reset();
        for(int i=input_peak_l;i<param_count;++i)if(parameters[i].kind==3)values[i].store(0);
#ifdef RETROLOOPER_FORCE_BOUNCE
        pickerProject=0;
        cancelNativePicker.store(true);
#endif
        configure();publish();publishVisuals();messageCode.store(Ready);return true;
    }
};

static Plugin& plugin(AEffect* e){return *static_cast<Plugin*>(e->object);}
static intptr_t dispatch(AEffect* e,int32_t op,int32_t index,intptr_t val,void* ptr,float opt) noexcept {
    auto& p=plugin(e);
    try {
        switch(op) {
            case effOpen:return 1;
            case effClose:delete &p;return 1;
            case effSetSampleRate:{std::lock_guard lock(p.gate);if(!std::isfinite(opt)||opt<8000||opt>192000)return 0;p.rate=opt;if(p.prepared)return p.prepare();return 1;}
            case effSetBlockSize:return val>0;
            case effMainsChanged:{std::lock_guard lock(p.gate);if(val&&!p.prepared&&!p.prepare())return 0;p.active.store(val!=0);p.engine->setRealtimeActive(val!=0);if(val){p.prepareRecordValue.store(0);p.protectPrepareRecord.store(true);}return 1;}
            case effStartProcess:return 1;
            case effStopProcess:return 1;
            case effSetProgram:return val==0;
            case effGetProgram:return 0;
            case effGetProgramName:case effGetProgramNameIndexed:text(ptr,"Default",24);return 1;
            case effGetParamName:if(index<0||index>=param_count)return 0;text(ptr,parameters[index].name,8);return 1;
            case effGetParamLabel:text(ptr,"",8);return 1;
            case effGetParamDisplay:if(index<0||index>=param_count)return 0;p.display(index,ptr);return 1;
            case effCanBeAutomated:return index>=0&&index<param_count&&!retired(index); // MPC needs visible readouts in its live cache; retired controls stay hidden.
            case effGetChunk:return p.save(ptr);
            case effSetChunk:if(p.load(ptr,val))return 1;p.messageCode.store(InvalidState);return 0;
            case effProcessEvents: {
                if(!ptr)return 0;const auto* list=static_cast<const VstEvents*>(ptr);if(list->numEvents<0||list->numEvents>4096)return 0;
                for(int i=0;i<list->numEvents;++i){const auto* event=list->events[i];if(!event||event->type!=1||event->byteSize<static_cast<int>(sizeof(VstMidiEvent)))continue;
                    const auto* m=reinterpret_cast<const VstMidiEvent*>(event);if(!p.midi.push({m->deltaFrames,m->midiData[0],m->midiData[1],m->midiData[2]}))p.messageCode.store(QueueFull);}
                return 1;
            }
            case effGetPlugCategory:return 1;
            case effGetEffectName:case effGetProductString:text(ptr,"Retrolooper One",32);return 1;
            case effGetVendorString:text(ptr,"RetroLooper",64);return 1;
            case effGetVendorVersion:return kVersion;
            case effGetVstVersion:return 2400;
            case effGetTailSize:return std::numeric_limits<int32_t>::max(); // Loops keep sounding after the input stops.
            case effCanDo:return ptr&&(!std::strcmp(static_cast<const char*>(ptr),"receiveVstEvents")||!std::strcmp(static_cast<const char*>(ptr),"receiveVstMidiEvent")||!std::strcmp(static_cast<const char*>(ptr),"receiveVstTimeInfo"))?1:0;
            default:return 0;
        }
    } catch(...) {p.messageCode.store(FileError);return 0;}
}
}
#ifdef RETROLOOPER_TESTING
extern "C" void rlo_test_conform_delay(int ms){akai::testConformDelay.store(ms);}
extern "C" uint64_t rlo_test_fallback_frames(AEffect* fx){return akai::plugin(fx).testFallbackTotal.load();}
#endif
extern "C" __attribute__((visibility("default"))) AEffect* VSTPluginMain(HostCallback host) noexcept {
    try {
        auto p=std::make_unique<akai::Plugin>(host);auto& e=p->fx;
        e.magic=0x56737450;e.dispatcher=akai::dispatch;e.object=p.get();e.numPrograms=1;e.numParams=akai::param_count;
        e.numInputs=2;e.numOutputs=2;e.flags=(1<<4)|(1<<5);e.uniqueID=akai::kUid;e.version=akai::kVersion;
        e.setParameter=[](AEffect* f,int32_t i,float v){akai::plugin(f).setFromHost(i,v);};
        e.getParameter=[](AEffect* f,int32_t i){return akai::plugin(f).normalized(i);};
        e.processReplacing=[](AEffect* f,float** in,float** out,int32_t n){akai::plugin(f).process(in,out,n);};
        e.process=[](AEffect* f,float** in,float** out,int32_t n){akai::plugin(f).process(in,out,n,false);};
        p.release();return &e;
    } catch(...) {return nullptr;}
}
