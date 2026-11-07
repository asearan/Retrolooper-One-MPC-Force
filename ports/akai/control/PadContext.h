#pragma once
#include <stdint.h>
/* MPC 3.10.0.49 EngineMode IDs, verified against the firmware's own
   ModeID-to-name switch at 0x165d4dc. Unknown modes/popups yield to MPC. */
static inline int rlo_pad_context_allowed(uint32_t context){
 unsigned mode=context>>16,popup=context&0xffff;
 if(popup==0x3080)return mode==0x2080; /* MAIN, no modal overlay. */
 if(popup!=0x30fa)return 0; /* Insert Effect Parameter Edit only. */
 return mode==0x2080||mode==0x2083||mode==0x2084||mode==0x208b;
}
