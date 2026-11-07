// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#include <stdexcept>
#include <vector>
namespace akai {
struct Audio { double rate=44100; std::vector<float> left,right; };
inline bool waveExists(const std::filesystem::path& path){return ::access(path.c_str(),F_OK)==0;}
inline uint32_t u32(const unsigned char* p) {return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
inline uint16_t u16(const unsigned char* p) {return uint16_t(p[0])|(uint16_t(p[1])<<8);}
inline void put32(std::ostream& s,uint32_t v) { for(int n=0;n<4;++n)s.put(char(v>>(8*n))); }
inline void put16(std::ostream& s,uint16_t v) {s.put(char(v));s.put(char(v>>8));}
inline Audio readWave(const std::filesystem::path& path) {
    std::ifstream f(path,std::ios::binary); if(!f)throw std::runtime_error("WAV not found");
    unsigned char h[12]; if(!f.read(reinterpret_cast<char*>(h),12)||std::memcmp(h,"RIFF",4)||std::memcmp(h+8,"WAVE",4))throw std::runtime_error("Invalid WAV");
    uint16_t format=0,channels=0,bits=0,align=0; uint32_t rate=0; std::vector<unsigned char> data;
    for(int chunks=0;f && chunks<1024;++chunks) {
        unsigned char c[8]; if(!f.read(reinterpret_cast<char*>(c),8))break;
        const uint32_t n=u32(c+4); if(n>200000000u)throw std::runtime_error("WAV too large");
        if(!std::memcmp(c,"fmt ",4)) {
            std::vector<unsigned char> b(n); if(n<16||!f.read(reinterpret_cast<char*>(b.data()),n))throw std::runtime_error("Invalid WAV fmt");
            format=u16(b.data());channels=u16(b.data()+2);rate=u32(b.data()+4);align=u16(b.data()+12);bits=u16(b.data()+14);
        } else if(!std::memcmp(c,"data",4)) {
            data.resize(n);if(!f.read(reinterpret_cast<char*>(data.data()),n))throw std::runtime_error("Truncated WAV");
        } else f.seekg(n,std::ios::cur);
        if(n&1)f.seekg(1,std::ios::cur);
    }
    if((channels!=1&&channels!=2)||rate<8000||rate>192000||!align||align!=channels*(bits/8)||data.size()%align ||
       !((format==1&&(bits==16||bits==24||bits==32))||(format==3&&bits==32)))throw std::runtime_error("Use PCM16/24/32 WAV");
    const auto frames=data.size()/align;if(frames>rate*120ull)throw std::runtime_error("WAV exceeds 120 sec");
    Audio a;a.rate=rate;a.left.resize(frames);a.right.resize(frames);
    auto sample=[&](const unsigned char* b) {
        float v=0;
        if(format==3) {uint32_t raw=u32(b);std::memcpy(&v,&raw,4);}
        else if(bits==16)v=static_cast<int16_t>(u16(b))/32768.f;
        else if(bits==24) {int32_t x=b[0]|(b[1]<<8)|(b[2]<<16);if(x&0x800000)x-=0x1000000;v=x/8388608.f;}
        else v=static_cast<int32_t>(u32(b))/2147483648.f;
        return std::isfinite(v)?v:0.f;
    };
    for(std::size_t i=0;i<frames;++i) {const auto* b=data.data()+i*align;a.left[i]=sample(b);a.right[i]=sample(b+(channels==2?bits/8:0));}
    return a;
}
inline void writeWave(const std::filesystem::path& path,const Audio& a) {
    if(a.left.size()!=a.right.size()||a.left.size()>a.rate*120||a.left.empty())throw std::runtime_error("No loop to export");
    // Never replace a user's file. Slots are explicitly chosen on the FILES page.
    if(waveExists(path))throw std::runtime_error("Slot exists; choose new");
    // ARM libc++ create_directories rejects an already-existing parent on this
    // firmware. Use mkdir directly and tolerate EEXIST at each parent level.
    const auto parent=path.parent_path().string();
    for(std::size_t i=1;i<=parent.size();++i)if(i==parent.size()||parent[i]=='/'){
        const auto folder=parent.substr(0,i);
        if(::mkdir(folder.c_str(),0755)!=0&&errno!=EEXIST)throw std::runtime_error("Cannot create WAV folder: "+std::string(std::strerror(errno)));
    }
    auto temp=path.string()+".tmp-XXXXXX";
    const int fd=::mkstemp(temp.data());if(fd<0)throw std::runtime_error("Cannot create WAV temporary file");::close(fd);
    std::ofstream f(temp,std::ios::binary|std::ios::trunc);if(!f)throw std::runtime_error("Cannot write WAV");
    const auto bytes=static_cast<uint32_t>(a.left.size()*6);
    f.write("RIFF",4);put32(f,36+bytes);f.write("WAVEfmt ",8);put32(f,16);put16(f,1);put16(f,2);
    put32(f,static_cast<uint32_t>(a.rate));put32(f,static_cast<uint32_t>(a.rate)*6);put16(f,6);put16(f,24);
    f.write("data",4);put32(f,bytes);
    // MPC-compatible PCM24. Only an over-full-scale render is scaled, uniformly
    // across both channels, so exporting a hot overdub cannot introduce clipping.
    float peak=1;for(const auto* c:{&a.left,&a.right})for(float v:*c)if(std::isfinite(v))peak=std::max(peak,std::abs(v));
    std::array<char,4096*6> packed{};
    for(std::size_t start=0;start<a.left.size();start+=4096){
        const auto n=std::min<std::size_t>(4096,a.left.size()-start);std::size_t at=0;
        for(std::size_t i=0;i<n;++i)for(float v:{a.left[start+i],a.right[start+i]}){
            const auto sample=std::clamp<int32_t>(std::isfinite(v)?static_cast<int32_t>(std::llround(v/peak*8388608.)):0,-8388608,8388607);
            for(int byte=0;byte<3;++byte)packed[at++]=static_cast<char>((static_cast<uint32_t>(sample)>>(byte*8))&255);
        }
        f.write(packed.data(),static_cast<std::streamsize>(at));
    }
    f.close();if(!f) {::unlink(temp.c_str());throw std::runtime_error("WAV write failed");}
    // Hard-link publication cannot replace a file another instance or process
    // created while this export was rendering. Internal MPC storage is ext4.
    if(::link(temp.c_str(),path.c_str())!=0){const int code=errno;::unlink(temp.c_str());throw std::runtime_error(code==EEXIST?"Slot exists; choose new":"Cannot publish WAV file");}
    ::unlink(temp.c_str());
}
}
