#include <cstdint>

extern "C" int rlo_force_load_audio_file(std::uintptr_t base, std::uint32_t engine, const char* path) noexcept {
    using StringConstructor=void(*)(std::uint32_t*,const char*);
    using StringDestructor=void(*)(std::uint32_t*);
    using FileConstructor=void(*)(std::uint32_t*,const std::uint32_t*);
    using LoadFile=void(*)(const std::uint32_t*,void*);
    struct NativeString {
        std::uint32_t data;
        StringDestructor destroy;
        ~NativeString(){destroy(&data);}
    };
    try {
        const auto construct=reinterpret_cast<StringConstructor>(base+0x983e60);
        const auto destroy=reinterpret_cast<StringDestructor>(base+0x934b88);
        const auto fileConstruct=reinterpret_cast<FileConstructor>(base+0x22dc35c);
        const auto load=reinterpret_cast<LoadFile>(base+0x1daafd8);
        std::uint32_t data=0;
        construct(&data,path);
        NativeString name{data,destroy};
        // Match Force's own one-String juce::File lifetime. Dispatching via
        // Project.Load lets the extension select an unrelated sample route.
        std::uint32_t fileData=0;
        fileConstruct(&fileData,&name.data);
        NativeString file{fileData,destroy};
        load(&file.data,reinterpret_cast<void*>(engine));
        return 1;
    } catch (...) {
        return 0;
    }
}
