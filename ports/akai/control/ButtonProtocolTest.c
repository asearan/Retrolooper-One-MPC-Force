#include "ButtonProtocol.h"
#include <assert.h>
#include <stdio.h>
int main(void){RloButtonParser p={0};
 unsigned char rec[]={0x90,73,127},up[]={0x90,73,0},dub[]={0x90,80,127},pad[]={0x99,73,127};
 assert(rlo_button_bytes(&p,rec,3)==1);assert(rlo_button_bytes(&p,rec,3)==0);assert(rlo_button_bytes(&p,up,3)==0);
 assert(rlo_button_bytes(&p,dub,3)==0);assert(p.capture_presses==2);
 assert(rlo_button_bytes(&p,dub,3)==0&&p.capture_presses==2);
 assert(rlo_button_bytes(&p,pad,3)==0&&p.capture_presses==2);
 assert(rlo_button_bytes(&p,rec,1)==0);assert(rlo_button_bytes(&p,rec+1,1)==0);assert(rlo_button_bytes(&p,rec+2,1)==1);
 unsigned char running[]={73,0,73,127,0xf8,73,0,73,127};assert(rlo_button_bytes(&p,running,sizeof running)==2);
 unsigned char sysex[]={0xf0,0x90,73,127,0xf8,0xf7};assert(rlo_button_bytes(&p,sysex,sizeof sysex)==0);
 unsigned char noteoff[]={0x80,73,0};assert(rlo_button_bytes(&p,noteoff,3)==0);assert(rlo_button_bytes(&p,rec,3)==1);
 puts("PASS: physical REC only; OVERDUB/pads excluded; held-key suppression, split packets, running status, realtime, SysEx and releases");
}
