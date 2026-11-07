#pragma once
#include <stdint.h>

typedef struct {
 uint32_t project;
 uint32_t track;
 uint32_t row;
 uint32_t clip;
 uint32_t lanes;
 uint32_t owner;
} RloForceClipTarget;

enum {
 RLO_CLIP_PENDING=0,
 RLO_CLIP_BOUNCED=1,
 RLO_CLIP_UNAVAILABLE=-1,
 RLO_CLIP_SELECTION_CHANGED=-2,
 RLO_CLIP_TRANSPORT_RUNNING=-3,
 RLO_CLIP_IMPORT_FAILED=-4,
 RLO_CLIP_BUSY=-5
};
