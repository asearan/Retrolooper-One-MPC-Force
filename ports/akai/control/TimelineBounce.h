#pragma once
#include "TimelineProtocol.h"

extern int rlo_mpc_insert_audio(uintptr_t base, uint32_t engine,
                               const RloMpcTimelineTarget *target, const char *path, int start_load);

static pthread_mutex_t timeline_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct {
    RloMpcTimelineTarget target;
    uint32_t id, effect, submitted;
    int running, result, abandoned, loading;
    char path[512];
} timeline_job;
static uint32_t timeline_serial;

/* Sequence's row-zero tree maps native Track pointers to their Arrange clips. */
static uint32_t timeline_arrangement(uint32_t sequence, uint32_t track) {
    if (!word_is(sequence, native_base + 0x6d50cc4)) return 0;
    uint32_t node = native_word(sequence + 0x528);
    for (unsigned depth = 0; node && depth < 32; ++depth) {
        uint32_t entry[7];
        if (!native_read(entry, node, sizeof entry)) return 0;
        if (entry[4] == track)
            return word_is(entry[5], native_base + 0x6d4a0f0) ? entry[5] : 0;
        node = entry[entry[4] < track ? 3 : 2];
    }
    return 0;
}

static int timeline_destination(uint32_t track, RloMpcTimelineTarget *target) {
    uint32_t sequence = native_word(native_project + 0xe4);
    uint32_t arrangement = timeline_arrangement(sequence, track);
    if (!arrangement || native_word(native_project + 0xa0) != track) return 0;
    *target = (RloMpcTimelineTarget){native_project, track, sequence, arrangement};
    return 1;
}

static uint32_t timeline_engine(void) {
#ifdef RLO_MPC_TIMELINE_TEST
    return 1;
#else
    uint32_t app = native_word(native_base + 0x6f35c88);
    if (!word_is(app, native_base + 0x6cdfaf4)) return 0;
    uint32_t window = native_word(app + 0xc4);
    if (!word_is(window, native_base + 0x6cdfe94)) return 0;
    uint32_t engine = native_word(window + 0x26c);
    if (!word_is(engine, native_base + 0x6ce5650) ||
        native_word(engine + 0x264) != native_project) return 0;
    return engine;
#endif
}

static void timeline_refresh(uint32_t track, const uint32_t *fx, unsigned count) {
    if (pthread_mutex_trylock(&timeline_mutex)) return;
    if (!timeline_job.id || timeline_job.running || timeline_job.result) {
        pthread_mutex_unlock(&timeline_mutex);
        return;
    }
    unsigned i = 0;
    while (i < count && fx[i] != timeline_job.effect) ++i;
    RloMpcTimelineTarget current;
    if (i == count ||
        !timeline_destination(track, &current) || memcmp(&current, &timeline_job.target, sizeof current)) {
        timeline_job.result = RLO_TIMELINE_CHANGED;
        pthread_mutex_unlock(&timeline_mutex);
        return;
    }
    if (now_ms() - timeline_job.submitted > 15000) {
        dprintf(STDERR_FILENO, "Retrolooper MPC bounce: sample load did not complete\n");
        timeline_job.result = RLO_TIMELINE_FAILED;
        pthread_mutex_unlock(&timeline_mutex);
        return;
    }
    int start_load = !timeline_job.loading;
    timeline_job.running = 1;
    pthread_mutex_unlock(&timeline_mutex);
    uint32_t engine = timeline_engine();
    int result = engine ? rlo_mpc_insert_audio(native_base, engine, &current, timeline_job.path, start_load)
                        : RLO_TIMELINE_UNAVAILABLE;
    pthread_mutex_lock(&timeline_mutex);
    timeline_job.running = 0;
    if (result == RLO_TIMELINE_LOADING) timeline_job.loading = 1;
    else timeline_job.result = result;
    if (timeline_job.abandoned) timeline_job.id = 0;
    pthread_mutex_unlock(&timeline_mutex);
}

__attribute__((visibility("default"))) int retrolooper_mpc_timeline_target(void *effect, RloMpcTimelineTarget *out) {
    RloTrackSnapshot view;
    if (!effect || !out || !__atomic_load_n(&native_supported, __ATOMIC_ACQUIRE) ||
        !snapshot_read(&view) || now_ms() - view.stamp > 100) return RLO_TIMELINE_UNAVAILABLE;
    for (unsigned i = 0; i < view.count; ++i) {
        if (view.fx[i] == (uintptr_t)effect && view.timeline.arrangement) {
            *out = view.timeline;
            return RLO_TIMELINE_BOUNCED;
        }
    }
    return RLO_TIMELINE_CHANGED;
}

__attribute__((visibility("default"))) int retrolooper_mpc_timeline_enqueue(void *effect,
        const RloMpcTimelineTarget *target, const char *path, uint32_t *ticket) {
    if (!effect || !target || !target->arrangement || !path || !ticket ||
        !__atomic_load_n(&native_supported, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&native_enabled, __ATOMIC_ACQUIRE)) return RLO_TIMELINE_UNAVAILABLE;
    /* The effect captured this destination before rendering. Recheck it on
       the UI thread in timeline_refresh, where the live graph is available.
       A delayed snapshot on the export worker cannot safely decide ownership. */
    size_t size = strnlen(path, sizeof timeline_job.path);
    if (size < 5 || size == sizeof timeline_job.path || path[0] != '/' ||
        strcmp(path + size - 4, ".wav")) return RLO_TIMELINE_FAILED;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return RLO_TIMELINE_FAILED;
    struct stat64 info;
    uint8_t wav[44];
    int valid = !fstat64(fd, &info) && S_ISREG(info.st_mode) && read(fd, wav, sizeof wav) == sizeof wav;
    close(fd);
    if (!valid || memcmp(wav, "RIFF", 4) || memcmp(wav+8, "WAVEfmt ", 8) ||
        memcmp(wav+16, "\020\0\0\0\001\0\002\0", 8) ||
        memcmp(wav+32, "\006\0\030\0data", 8)) return RLO_TIMELINE_FAILED;
    uint32_t riff, rate, data;
    memcpy(&riff, wav+4, 4);
    memcpy(&rate, wav+24, 4);
    memcpy(&data, wav+40, 4);
    if ((rate != 44100 && rate != 48000) || !data || data % 6 || data > rate * 120u * 6u ||
        info.st_size != data+44u || riff != data+36u) return RLO_TIMELINE_FAILED;
    pthread_mutex_lock(&timeline_mutex);
    if (timeline_job.id) {
        pthread_mutex_unlock(&timeline_mutex);
        return RLO_TIMELINE_BUSY;
    }
    if (!++timeline_serial) ++timeline_serial;
    *ticket = timeline_job.id = timeline_serial;
    timeline_job.effect = (uintptr_t)effect;
    timeline_job.target = *target;
    timeline_job.submitted = now_ms();
    timeline_job.running = timeline_job.result = timeline_job.abandoned = timeline_job.loading = 0;
    memcpy(timeline_job.path, path, size + 1);
    pthread_mutex_unlock(&timeline_mutex);
    return RLO_TIMELINE_PENDING;
}

__attribute__((visibility("default"))) int retrolooper_mpc_timeline_result(uint32_t ticket, int cancel) {
    pthread_mutex_lock(&timeline_mutex);
    int result = RLO_TIMELINE_UNAVAILABLE;
    if (ticket && ticket == timeline_job.id) {
        result = timeline_job.result;
        if (cancel && timeline_job.running) timeline_job.abandoned = 1;
        else if (cancel || result) timeline_job.id = 0;
    }
    pthread_mutex_unlock(&timeline_mutex);
    return result;
}
