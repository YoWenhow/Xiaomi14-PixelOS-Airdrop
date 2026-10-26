#include "rate-compat.h"
extern void station_trace(int,const void *,unsigned long,int);
extern void *dlsym(void *,const char *);
extern int getsockname(int,void *,unsigned *);
extern int getsockopt(int,int,int,void *,unsigned *);
static const void *convert(int fd,const void *buf,unsigned long len,u8 copy[4096]) {
    if(len>4096||len<20)return buf;
    u8 address[128];unsigned alen=sizeof(address);int protocol=0;unsigned plen=sizeof(protocol);
    if(getsockname(fd,address,&alen)||alen<2||read16(address)!=16||
       getsockopt(fd,1,38,&protocol,&plen)||plen!=sizeof(protocol)||protocol!=16)return buf;
    station_trace(fd,buf,len,1);
    for(unsigned long i=0;i<len;i++)copy[i]=((const u8 *)buf)[i];
    if(len>=20&&copy[16]==103) {
        u8 *sub=nla(copy+20,(unsigned)len-20,196,4);
        if(sub&&read32(sub)==3) {
            char hex[560];unsigned n=0;const char *digits="0123456789abcdef",*prefix="Stage37 rate request hex=";
            while(*prefix)hex[n++]=*prefix++;
            for(unsigned i=0;i<len&&i<256;i++){hex[n++]=digits[copy[i]>>4];hex[n++]=digits[copy[i]&15];}
            hex[n++]='\n';
            register long x0 __asm__("x0")=2,x1 __asm__("x1")=(long)hex,x2 __asm__("x2")=n,x8 __asm__("x8")=64;
            __asm__ volatile("svc 0":"+r"(x0):"r"(x1),"r"(x2),"r"(x8):"memory","cc");
        }
    }
    int changed=rewrite_rate(copy,(unsigned)len);if(!changed)return buf;
    const char *message=changed==3?
        "Stage77: explicit 5745 MHz bandwidth fallback 80 to 20 MHz\n":
        changed==2?"Stage81: observed VHT80 rate mapped to HT20 NSS1 MCS7 GI-default\n":
        "Stage37: explicit diagnostic rate fallback HT20 NSS1 MCS0 GI-default\n";
    unsigned message_len=0;while(message[message_len])message_len++;
    register long x0 __asm__("x0")=2,x1 __asm__("x1")=(long)message,x2 __asm__("x2")=message_len,x8 __asm__("x8")=64;
    __asm__ volatile("svc 0":"+r"(x0):"r"(x1),"r"(x2),"r"(x8):"memory","cc");
    return copy;
}
long send(int fd,const void *buf,unsigned long len,int flags) {
    typedef long(*Fn)(int,const void *,unsigned long,int);
    Fn original=(Fn)dlsym((void *)-1,"send");if(!original)return -1;
    u8 copy[4096];return original(fd,convert(fd,buf,len,copy),len,flags);
}
long sendto(int fd,const void *buf,unsigned long len,int flags,const void *address,unsigned length) {
    typedef long(*Fn)(int,const void *,unsigned long,int,const void *,unsigned);
    Fn original=(Fn)dlsym((void *)-1,"sendto");if(!original)return -1;
    u8 copy[4096];return original(fd,convert(fd,buf,len,copy),len,flags,address,length);
}
