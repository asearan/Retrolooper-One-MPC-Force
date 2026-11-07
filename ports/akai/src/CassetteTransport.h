#pragma once
#include <algorithm>
#include <cmath>

namespace akai {
// Visual transport only: no effect on audio speed, pitch, position or timing.
// Angular velocity follows v/r, and tape area transfers between the two packs.
// A long virtual tape keeps pack changes gradual on short, repeating loops.
struct CassetteTransport {
    static constexpr double coreRadius=.011,outerRadius=.022,tapeSpeed=.0476;
    static constexpr double tau=6.2831853071795864769;
    double supplyPhase=0,takeupPhase=0,fill=.25;
    static double radius(double amount) noexcept {
        return std::sqrt(coreRadius*coreRadius+(outerRadius*outerRadius-coreRadius*coreRadius)*amount);
    }
    void advance(double dt,bool running) noexcept {
        if(!running||!std::isfinite(dt)||dt<=0)return;
        // Keep a small visual reserve in very long sessions; there is no tape-end
        // behavior in this digital looper. Ordinary loop wraps never rewind it.
        const double next=std::min(.90,fill+dt/3600.);
        const double middle=(fill+next)*.5;
        supplyPhase=std::fmod(supplyPhase+tapeSpeed*dt/(tau*radius(1-middle)),1.);
        takeupPhase=std::fmod(takeupPhase+tapeSpeed*dt/(tau*radius(middle)),1.);
        fill=next;
    }
};
}
