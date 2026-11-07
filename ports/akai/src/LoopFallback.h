// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "WaveFile.h"
#include <algorithm>
#include <cmath>
namespace akai {
// Immutable playback snapshot used only while the worker grows/conforms loop
// storage. This keeps the loop audible when the engine lock is held off-thread.
struct LoopFallback {
    Audio audio;
    std::size_t frames=0;
    double position=0,reference=120;
    bool audible=true,repeat=true;
    float integerSample(int channel,std::size_t pos,int crossfade) const {
        const auto& data=channel?audio.right:audio.left;
        if(data.empty()||!frames)return 0;
        const auto raw=[&](std::size_t i){return i<data.size()?data[i]:repeat?data[i%data.size()]:0.f;};
        float sample=raw(pos);
        if(crossfade>0&&frames>=4){
            const auto xf=std::max<std::size_t>(2,std::min<std::size_t>(crossfade,frames/2));
            if(pos<xf||pos>=frames-xf){
                const float first=raw(0),last=raw(frames-1),join=(first+last)*.5f;
                const bool head=pos<xf;const auto distance=head?pos:frames-1-pos;
                const float t=static_cast<float>(distance)/static_cast<float>(xf-1);
                sample+=(join-(head?first:last))*(1-t*t*(3-2*t));
            }
        }
        return sample;
    }
    float sample(int channel,double p,int crossfade) const {
        if(!frames)return 0;
        p=std::fmod(p,static_cast<double>(frames));if(p<0)p+=frames;
        const auto i=static_cast<std::size_t>(p);const float f=static_cast<float>(p-i);
        const auto a=integerSample(channel,i,crossfade);
        return f==0?a:a+(integerSample(channel,(i+1)%frames,crossfade)-a)*f;
    }
};
}
