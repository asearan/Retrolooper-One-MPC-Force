#pragma once
// VST2 ABI declarations derived from sd88me/mpc-vst-plugins (MIT).
// See vendor/mpc-vst-plugins-LICENSE. No Steinberg SDK is required.
#include <cstdint>
struct AEffect;
using HostCallback = intptr_t (*)(AEffect*,int32_t,int32_t,intptr_t,void*,float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect*,int32_t,int32_t,intptr_t,void*,float);
    void (*process)(AEffect*,float**,float**,int32_t);
    void (*setParameter)(AEffect*,int32_t,float);
    float (*getParameter)(AEffect*,int32_t);
    int32_t numPrograms,numParams,numInputs,numOutputs,flags;
    intptr_t resvd1,resvd2;
    int32_t initialDelay,realQualities,offQualities;
    float ioRatio;
    void *object,*user;
    int32_t uniqueID,version;
    void (*processReplacing)(AEffect*,float**,float**,int32_t);
    void (*processDoubleReplacing)(AEffect*,double**,double**,int32_t);
    char future[56];
};
struct VstEvent { int32_t type,byteSize,deltaFrames,flags; char data[16]; };
struct VstMidiEvent {
    int32_t type,byteSize,deltaFrames,flags,noteLength,noteOffset;
    uint8_t midiData[4]; char detune,noteOffVelocity,reserved1,reserved2;
};
struct VstEvents { int32_t numEvents; intptr_t reserved; VstEvent* events[2]; };
struct VstTimeInfo {
    double samplePos,sampleRate,nanoSeconds,ppqPos,tempo,barStartPos,cycleStartPos,cycleEndPos;
    int32_t timeSigNumerator,timeSigDenominator,smpteOffset,smpteFrameRate,samplesToNextClock,flags;
};
enum { effOpen=0,effClose=1,effSetProgram=2,effGetProgram=3,effGetProgramName=5,
    effGetParamLabel=6,effGetParamDisplay=7,effGetParamName=8,effSetSampleRate=10,
    effSetBlockSize=11,effMainsChanged=12,effGetChunk=23,effSetChunk=24,effProcessEvents=25,
    effCanBeAutomated=26,effGetProgramNameIndexed=29,effGetPlugCategory=35,
    effGetEffectName=45,effGetVendorString=47,effGetProductString=48,
    effGetVendorVersion=49,effCanDo=51,effGetTailSize=52,effGetVstVersion=58,effStartProcess=71,effStopProcess=72 };
enum { audioMasterAutomate=0,audioMasterGetTime=7,audioMasterUpdateDisplay=42,
    kVstTransportChanged=1,kVstTransportPlaying=2,kVstTransportCycleActive=4,kVstTransportRecording=8,kVstPpqPosValid=1<<9,
    kVstTempoValid=1<<10,kVstCyclePosValid=1<<12,kVstTimeSigValid=1<<13 };
