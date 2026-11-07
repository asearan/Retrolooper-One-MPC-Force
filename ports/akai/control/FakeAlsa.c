// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#include <string.h>
#include <stddef.h>
#include <sys/types.h>
static unsigned char payload[128];static ssize_t result;
static unsigned char written[256];static size_t written_size;
void fake_payload(const unsigned char*b,size_t n){memcpy(payload,b,n);result=(ssize_t)n;}
void fake_error(void){result=-11;}
int snd_rawmidi_open(void**in,void**out,const char*name,int mode){(void)mode;if(in)*in=(void*)(strstr(name,"Private")?0x101:0x202);if(out)*out=(void*)0x303;return 0;}
ssize_t snd_rawmidi_read(void*h,void*b,size_t n){(void)h;if(result<0)return result;if(n<(size_t)result)return -22;memcpy(b,payload,(size_t)result);return result;}
int snd_rawmidi_close(void*h){(void)h;return 0;}
ssize_t snd_rawmidi_write(void*h,const void*b,size_t n){(void)h;if(n>sizeof written)return -22;memcpy(written,b,n);written_size=n;return (ssize_t)n;}
const unsigned char*fake_written(size_t*n){*n=written_size;return written;}
size_t snd_rawmidi_info_sizeof(void){return sizeof(void*);}
int snd_rawmidi_info(void*h,void*info){memcpy(info,&h,sizeof h);return 0;}
const char*snd_rawmidi_info_get_name(const void*i){(void)i;return "MPC One +";}
const char*snd_rawmidi_info_get_subdevice_name(const void*i){void*h=0;memcpy(&h,i,sizeof h);return h==(void*)0x101||h==(void*)0x303?"MPC Studio Live Private":"External MIDI";}
