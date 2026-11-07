#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#define CHECK(x) do{if(!(x)){dprintf(2,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 int(*open_)(void**,void**,const char*,int)=dlsym(RTLD_DEFAULT,"snd_rawmidi_open");
 ssize_t(*read_)(void*,void*,size_t)=dlsym(RTLD_DEFAULT,"snd_rawmidi_read");
 int(*close_)(void*)=dlsym(RTLD_DEFAULT,"snd_rawmidi_close");
 ssize_t(*write_)(void*,const void*,size_t)=dlsym(RTLD_DEFAULT,"snd_rawmidi_write");
 const unsigned char*(*written_)(size_t*)=dlsym(RTLD_DEFAULT,"fake_written");
 void(*feed)(const unsigned char*,size_t)=dlsym(RTLD_DEFAULT,"fake_payload");
 void(*error_)(void)=dlsym(RTLD_DEFAULT,"fake_error");
 uint32_t(*gen)(void)=dlsym(RTLD_DEFAULT,"retrolooper_mpc_rec_generation");
 uint32_t(*ready)(void)=dlsym(RTLD_DEFAULT,"retrolooper_mpc_button_ready");
 CHECK(open_&&read_&&close_&&write_&&written_&&feed&&error_&&gen&&ready);void*private_=0,*public_=0,*output_=0;
 CHECK(open_(0,&output_,"Private",0)==0);const unsigned char outputBytes[]={0xf0,0x47,0x7f,0x3b,0x65,0,4,0,1,2,3,0xf7};
 CHECK(write_(output_,outputBytes,sizeof outputBytes)==sizeof outputBytes);size_t writtenSize=0;const unsigned char*written=written_(&writtenSize);CHECK(writtenSize==sizeof outputBytes&&!memcmp(written,outputBytes,writtenSize));
 // This executable is deliberately not the exact supported Akai host.
 CHECK(open_(&private_,0,"Private",0)==0&&ready()==0);unsigned char rec[]={0x90,73,127},off[]={0x90,73,0},dub[]={0x90,80,127},out[128];
 feed(rec,3);CHECK(read_(private_,out,sizeof out)==3&&memcmp(rec,out,3)==0&&gen()==0);
 feed(off,3);CHECK(read_(private_,out,sizeof out)==3&&memcmp(off,out,3)==0&&gen()==0);
 feed(dub,3);CHECK(read_(private_,out,sizeof out)==3&&memcmp(dub,out,3)==0&&gen()==0);
 CHECK(open_(&public_,0,"Public",0)==0);feed(rec,3);CHECK(read_(public_,out,sizeof out)==3&&memcmp(rec,out,3)==0&&gen()==0);
 error_();CHECK(read_(private_,out,sizeof out)==-11&&gen()==0);
 CHECK(close_(public_)==0&&ready()==0);CHECK(close_(private_)==0&&ready()==0);CHECK(close_(output_)==0);
 dprintf(2,"PASS: an unsupported host cannot activate native Controls; MIDI bytes, output and ALSA errors pass through\n");return 0;
}
