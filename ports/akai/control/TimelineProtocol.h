#pragma once
#include <stdint.h>

typedef struct {
    uint32_t project;
    uint32_t track;
    uint32_t sequence;
    uint32_t arrangement;
} RloMpcTimelineTarget;

enum {
    RLO_TIMELINE_PENDING = 0,
    RLO_TIMELINE_BOUNCED = 1,
    RLO_TIMELINE_LOADING = 2,
    RLO_TIMELINE_UNAVAILABLE = -1,
    RLO_TIMELINE_CHANGED = -2,
    RLO_TIMELINE_FAILED = -3,
    RLO_TIMELINE_BUSY = -4,
    RLO_TIMELINE_RUNNING = -5,
    RLO_TIMELINE_OCCUPIED = -6
};
