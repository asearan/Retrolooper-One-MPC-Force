// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <limits>

namespace retrolooper {
enum class PluginMidiGesture { Tap=0, DoubleTap=1, Hold=2 };
inline const char* pluginMidiGestureName(PluginMidiGesture gesture) noexcept {
    return gesture==PluginMidiGesture::DoubleTap?"Double tap":gesture==PluginMidiGesture::Hold?"Hold":"Tap";
}
// Audio-thread owned, bounded storage. Time is the continuous processed sample clock,
// independent of host seeks/tempo; deadlines retain their exact offset within a block.
class PluginMidiGestures {
    static constexpr auto never=std::numeric_limits<std::int64_t>::max();
    struct Press {
        std::int64_t downAt=0,releasedAt=0;
        int mask=0;
        bool down=false,pending=false,held=false;
    };
    std::array<Press,16*128> presses_{};
    std::int64_t next_=never,tapMax_=16800,doubleWindow_=21600,hold_=24000;
    std::int64_t deadline(const Press& p) const noexcept {
        if(p.down && !p.held && (p.mask&4))return p.downAt+hold_;
        if(!p.down && p.pending)return p.releasedAt+doubleWindow_;
        return never;
    }
public:
    void prepare(double rate) noexcept {
        tapMax_=static_cast<std::int64_t>(rate*.35+.5);doubleWindow_=static_cast<std::int64_t>(rate*.45+.5);hold_=static_cast<std::int64_t>(rate*.5+.5);reset();
    }
    void reset() noexcept {presses_.fill({});next_=never;}
    void resetChannel(int channel) noexcept {for(int n=0;n<128;++n)presses_[static_cast<std::size_t>(channel*128+n)]={};}
    template<class Fire> void advance(std::int64_t until,Fire&& fire) noexcept {
        if(until<next_)return;
        next_=never;
        for(std::size_t i=0;i<presses_.size();++i) {
            auto& p=presses_[i];const auto due=deadline(p);
            if(due<=until) {
                if(p.down) {p.held=true;p.pending=false;fire(static_cast<int>(i%128),PluginMidiGesture::Hold,due);}
                else {if(p.mask&1)fire(static_cast<int>(i%128),PluginMidiGesture::Tap,due);p={};}
            }
            next_=std::min(next_,deadline(p));
        }
    }
    template<class Fire> void message(int channel,int note,bool down,std::int64_t at,int mask,Fire&& fire) noexcept {
        advance(at,fire);
        if(mask==1) {if(down)fire(note,PluginMidiGesture::Tap,at);return;} // Existing tap-only/latching pedals stay immediate.
        auto& p=presses_[static_cast<std::size_t>(channel*128+note)];
        if(down) {
            if(p.down)return; // Repeated note-on while held is not another physical gesture.
            p.down=true;p.held=false;p.downAt=at;p.mask=mask;
        } else {
            if(!p.down)return;
            p.down=false;
            if(p.held) {p={};return;}
            const auto duration=at-p.downAt;
            if((mask&2) && duration<=tapMax_) {
                if(p.pending && at-p.releasedAt<=doubleWindow_) {fire(note,PluginMidiGesture::DoubleTap,at);p={};return;}
                if(p.pending && (mask&1))fire(note,PluginMidiGesture::Tap,at);
                p.pending=true;p.releasedAt=at;
            } else {
                if(mask&1)fire(note,PluginMidiGesture::Tap,at);
                p={};
            }
        }
        next_=std::min(next_,deadline(p));
    }
};
}
