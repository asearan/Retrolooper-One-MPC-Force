#pragma once
#include <stdint.h>
/* Status is a bitmask; an unknown or stale native model never grants capture. */
enum { RLO_TRACK_SUPPORTED=1, RLO_TRACK_READY=2, RLO_TRACK_SELECTED=4, RLO_TRACK_DISARMED=8,
       RLO_TRACK_MONITOR_ROUTED=16,RLO_TRACK_INPUT_READY=32 };

