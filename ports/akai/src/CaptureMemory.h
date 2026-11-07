#pragma once
#include <cstddef>
#include <cstdio>

namespace akai {
// Live stereo float audio + nine page-history banks. Reserve room for one
// channel's grow/copy peak and leave the host 128 MiB for its own work.
inline std::size_t captureGrowthBytes(std::size_t current,std::size_t wanted) {
    return wanted>current?(wanted-current)*80+ wanted*sizeof(float):0;
}
inline bool captureBudgetFits(std::size_t current,std::size_t wanted,std::size_t available) {
    if(wanted<=current)return true;
    constexpr std::size_t hostReserve=128u*1024*1024;
    return available>=hostReserve&&captureGrowthBytes(current,wanted)<=available-hostReserve;
}
inline bool captureMemoryAvailable(std::size_t current,std::size_t wanted) {
#if defined(__linux__)
    // Worker thread only. MemAvailable includes reclaimable caches; MemFree
    // alone underestimates usable RAM on this firmware.
    auto* file=std::fopen("/proc/meminfo","r");if(!file)return false;
    char line[128];std::size_t kb=0;bool found=false;
    while(std::fgets(line,sizeof line,file))if(std::sscanf(line,"MemAvailable: %zu kB",&kb)==1){found=true;break;}
    std::fclose(file);
    return found&&captureBudgetFits(current,wanted,kb*1024);
#else
    (void)current;(void)wanted;return true;
#endif
}
}
