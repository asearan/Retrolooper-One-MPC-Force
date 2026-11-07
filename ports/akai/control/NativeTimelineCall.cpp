#include "TimelineProtocol.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {
using Word = std::uint32_t;
using Address = std::uintptr_t;

Word word(Address address) {
    return *reinterpret_cast<const Word*>(address);
}

template<class Function>
Function at(Address base, Address offset) {
    return reinterpret_cast<Function>(base + offset);
}

struct NativeRef {
    Word object = 0;
    Word control = 0;
};

struct RefGuard {
    Address base;
    NativeRef ref{};
    ~RefGuard() {
        if (ref.control) at<void(*)(Word)>(base, 0xcb9c3c)(ref.control);
    }
};

struct NativeBuffer {
    Address base, cleanup;
    alignas(8) Word words[32]{};
    bool initialized = false;
    ~NativeBuffer() {
        if (initialized) at<void(*)(void*)>(base, cleanup)(words);
    }
};

/* Pinned 3.10.0.49 ABI. The existing Audio Region insert uses a
   shared AudioEventEditContextImpl and a native undoable command. */
struct RegionContextStorage {
    Word sharedVtable, strong, weak;
    Word contextVtable;
    NativeRef arrangement;
    Word tempo;
};
static_assert(sizeof(RegionContextStorage) == 28);
}

extern "C" int rlo_mpc_insert_audio(Address base, Word engine,
        const RloMpcTimelineTarget* target, const char* path, int startLoad) noexcept {
    try {
        if (word(base + 0x983ea0) != 0xe16d41f0 ||
            word(base + 0x22dc734) != 0xe59f21e0 ||
            word(base + 0x16e3b6c) != 0xe16d41f0 ||
            word(base + 0x1da4520) != 0xe59f216c ||
            word(base + 0x29419c8) != 0xe52d4008 ||
            word(base + 0x274f774) != 0xe16d41fc ||
            word(base + 0x2757a28) != 0xe16d41fc ||
            word(base + 0x26e9300) != 0xe52d4008)
            return RLO_TIMELINE_UNAVAILABLE;

        const auto& destination = *target;
        const Word project = destination.project;
        const Word playhead = word(engine + 0x274);
        if (!playhead || word(playhead) != base + 0x6c9ce0c)
            return RLO_TIMELINE_UNAVAILABLE;
        if (word(playhead + 0x44)) return RLO_TIMELINE_RUNNING;
        const Word arrangement = destination.arrangement;

        const Word pool = word(project + 0xb14);
        const Word commands = word(project + 0xb38);
        const Word clock = word(project + 0xb44);
        const Word factory = word(engine + 0x340);
        const Word queues = word(engine + 0x30c);
        const Word transport = queues ? word(queues + 0x3b0) : 0;
        if (!pool || word(pool) != base + 0x6cb659c ||
            !commands || word(commands) != base + 0x6ca2bf4 ||
            !clock || !transport || !factory || word(factory) != base + 0x6ca73bc ||
            word(factory + 0x14) != project || word(factory + 0x30) != commands)
            return RLO_TIMELINE_UNAVAILABLE;
        const Word tempo = transport + 0xe8;
        const float bpm = *reinterpret_cast<const float*>(tempo + 0xb4);
        if (!std::isfinite(bpm) || bpm < 10 || bpm > 999) return RLO_TIMELINE_UNAVAILABLE;

        NativeBuffer name{base, 0x934bc8};
        at<void(*)(void*, const char*)>(base, 0x983ea0)(name.words, path);
        name.initialized = true;
        NativeBuffer file{base, 0x934bc8};
        at<void(*)(void*, const void*)>(base, 0x22dc734)(file.words, name.words);
        file.initialized = true;
        if (startLoad) {
            NativeBuffer request{base, 0x16e3c48};
            at<void(*)(void*, const void*, unsigned)>(base, 0x16e3b6c)(request.words, file.words, 1);
            request.initialized = true;
            // The browser's Load Sample path decodes into the project pool.
            // AddSampleToPool only accepts a sample that has already been decoded.
            const Word storage = word(engine + 0x2e4), memory = word(engine + 0xf8);
            const Word mode = word(engine + 0x118), progress = word(engine + 0x144), listener = word(engine + 0x148);
            if (!storage || !memory || !mode || !progress || !listener) return RLO_TIMELINE_UNAVAILABLE;
            const Word browser = word(engine + 0x34c), tasks = word(engine + 0x13c);
            if (!browser || !tasks) return RLO_TIMELINE_UNAVAILABLE;
            dprintf(STDERR_FILENO, "Retrolooper MPC bounce: requesting native WAV load\n");
            at<void(*)(void*, Word, Word, Word, Word, Word, Word, Word, Word, Word, unsigned)>(
                base, 0x1da4520)(request.words, browser + 0x78, tasks, project, storage, memory, mode, progress, listener, factory, 0);
            return RLO_TIMELINE_LOADING;
        }
        if (word(project + 0xa0) != destination.track || word(project + 0xe4) != destination.sequence)
            return RLO_TIMELINE_CHANGED;
        if (word(playhead + 0x44)) return RLO_TIMELINE_RUNNING;

        RefGuard sample{base};
        at<void(*)(NativeRef*, Word, const void*)>(base, 0x2716c68)(&sample.ref, pool, file.words);
        if (!sample.ref.object || !sample.ref.control) return RLO_TIMELINE_LOADING;

        NativeBuffer region{base, 0x275e204};
        at<void(*)(void*, const NativeRef*, std::int64_t, float, unsigned)>(base, 0x274f774)(
            region.words, &sample.ref, 0, bpm, 0);
        region.initialized = true;
        if ((region.words[2] & 0xffu) != 4 || !region.words[4]) return RLO_TIMELINE_FAILED;
        const Word* event = region.words;
        const auto end = at<std::int64_t(*)(const Word**, float)>(base, 0x26e9300)(&event, bpm);
        if (end <= 0) return RLO_TIMELINE_FAILED;
        const Word previousEvents = word(arrangement + 0x3fc);
        if (previousEvents) {
            const Word first = word(arrangement + 0x3f4);
            if (!first || first == arrangement + 0x3ec) return RLO_TIMELINE_FAILED;
            std::int64_t firstStart;
            std::memcpy(&firstStart, reinterpret_cast<const void*>(first + 0x10), sizeof firstStart);
            if (firstStart < end) return RLO_TIMELINE_OCCUPIED;
        }

        RefGuard clip{base};
        at<void(*)(NativeRef*, Word, Word, Word)>(base, 0x29419c8)(
            &clip.ref, destination.sequence, destination.track, 0);
        if (clip.ref.object != arrangement || !clip.ref.control) return RLO_TIMELINE_CHANGED;
        auto* storage = static_cast<RegionContextStorage*>(
            at<void*(*)(unsigned)>(base, 0x2c28cc0)(sizeof(RegionContextStorage)));
        *storage = {Word(base + 0x6c9e3c0), 1, 1, Word(base + 0x6cb6a18), clip.ref, tempo};
        clip.ref = {};
        RefGuard context{base, {Word(reinterpret_cast<Address>(&storage->contextVtable)),
                                Word(reinterpret_cast<Address>(storage))}};

        // MPC's single-event command owns the temporary event tree. Its
        // result is an AudioEvent, not the tree returned by the bulk command.
        NativeBuffer result{base, 0x275e204};
        dprintf(STDERR_FILENO, "Retrolooper MPC bounce: WAV ready, inserting one audio region\n");
        at<void(*)(void*, Word, const NativeRef*, const void*, Word)>(base, 0x2757a28)(
            result.words, clock, &context.ref, region.words, commands);
        result.initialized = true;
        const bool inserted = word(arrangement + 0x3fc) > previousEvents;
        dprintf(STDERR_FILENO, "Retrolooper MPC bounce: WAV loaded, timeline region inserted=%d\n", inserted);
        return inserted ? RLO_TIMELINE_BOUNCED : RLO_TIMELINE_FAILED;
    } catch (...) {
        return RLO_TIMELINE_FAILED;
    }
}
