// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace akai {
// Stereo 12 dB/octave Butterworth filters on loop playback, before dry input is
// mixed by the host insert. TPT state variables stay stable while knobs move.
class LoopFilters {
    struct Stage {
        double g=0,targetG=0,mix=0,targetMix=0;
        std::array<double,2> s1{},s2{};
        void reset(){s1.fill(0);s2.fill(0);mix=0;}
        void process(float& l,float& r,double smoothing,bool high){
            g+=(targetG-g)*smoothing;mix+=(targetMix-mix)*smoothing;
            if(targetMix==0&&mix<1.e-6){mix=0;s1.fill(0);s2.fill(0);return;}
            constexpr double k=1.4142135623730951;
            const double a1=1/(1+g*(g+k)),a2=g*a1,a3=g*a2;
            auto channel=[&](float x,int c){
                const double v3=x-s2[c],v1=a1*s1[c]+a2*v3,v2=s2[c]+a2*s1[c]+a3*v3;
                s1[c]=2*v1-s1[c];s2[c]=2*v2-s2[c];
                const double filtered=high?x-k*v1-v2:v2;
                return static_cast<float>(x+mix*(filtered-x));
            };
            l=channel(l,0);r=channel(r,1);
        }
    } hp,lp;
    double rate=44100,smoothing=0;
public:
    static double hz(float normalized){return 20*std::pow(1000.,std::clamp(normalized,0.f,1.f));}
    void prepare(double sr){rate=sr;smoothing=1-std::exp(-1/(.01*sr));hp=Stage{};lp=Stage{};set(0,1);hp.g=hp.targetG;lp.g=lp.targetG;}
    void reset(){hp.reset();lp.reset();}
    void set(float highPass,float lowPass){
        hp.targetG=std::tan(3.141592653589793*std::min(hz(highPass),rate*.45)/rate);
        lp.targetG=std::tan(3.141592653589793*std::min(hz(lowPass),rate*.45)/rate);
        hp.targetMix=highPass>.000001f?1:0;lp.targetMix=lowPass<.999999f?1:0;
    }
    void process(float& left,float& right){hp.process(left,right,smoothing,true);lp.process(left,right,smoothing,false);}
};
}
