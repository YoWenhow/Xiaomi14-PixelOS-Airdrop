#include "station-trace.h"
extern void *dlsym(void *,const char *);
extern int *__errno(void);
extern int getsockname(int,void *,unsigned *);
extern int getsockopt(int,int,int,void *,unsigned *);
typedef unsigned long long key64;
static key64 pending[16];
static unsigned next_slot,records;
static key64 key(int fd,unsigned seq){return ((key64)(unsigned)fd<<32)|seq;}
static int generic_netlink(int fd){
    s8 a[128];unsigned n=sizeof(a),plen=sizeof(int);int protocol=0;
    return !getsockname(fd,a,&n)&&n>=2&&st16(a)==16&&
        !getsockopt(fd,1,38,&protocol,&plen)&&plen==sizeof(int)&&protocol==16;
}
static void output(int fd,const s8 *p,unsigned n,int tx){
    if(__atomic_fetch_add(&records,1,__ATOMIC_RELAXED)>=64)return;
    char line[8300];unsigned used=0;const char *prefix=tx?"Stage46 station TX fd=":"Stage46 station ACK fd=";
    const char *hex="0123456789abcdef";
    while(*prefix)line[used++]=*prefix++;
    for(int shift=28;shift>=0;shift-=4)line[used++]=hex[((unsigned)fd>>shift)&15];
    prefix=" hex=";while(*prefix)line[used++]=*prefix++;
    if(n>4096)n=4096;
    for(unsigned i=0;i<n;i++){line[used++]=hex[p[i]>>4];line[used++]=hex[p[i]&15];}
    line[used++]='\n';
    register long x0 __asm__("x0")=2,x1 __asm__("x1")=(long)line,x2 __asm__("x2")=used,x8 __asm__("x8")=64;
    __asm__ volatile("svc 0":"+r"(x0):"r"(x1),"r"(x2),"r"(x8):"memory","cc");
}
void station_trace(int fd,const void *buffer,unsigned long length,int tx){
    int saved=*__errno();
    const s8 *b=buffer;
    if(length<16||length>65536||!generic_netlink(fd))goto done;
    if(tx){
        if(!station_request(b,(unsigned)length))goto done;
        unsigned slot=__atomic_fetch_add(&next_slot,1,__ATOMIC_RELAXED)%16;
        __atomic_store_n(&pending[slot],key(fd,st32(b+8)),__ATOMIC_RELEASE);
        output(fd,b,(unsigned)length,1);
    }else{
        unsigned off=0;
        while(length-off>=16){
            unsigned size=st32(b+off),step=(size+3)&~3u;
            if(size<16||size>length-off||step<size)break;
            if(st16(b+off+4)==2&&size>=36){
                key64 wanted=key(fd,st32(b+off+8));
                for(unsigned i=0;i<16;i++){
                    if(wanted&&__atomic_load_n(&pending[i],__ATOMIC_ACQUIRE)==wanted){
                        output(fd,b+off,size,0);break;
                    }
                }
            }
            if(step>length-off)break;
            off+=step;
        }
    }
done:*__errno()=saved;
}
long recv(int fd,void *buf,unsigned long len,int flags){
    typedef long(*Fn)(int,void *,unsigned long,int);
    Fn fn=(Fn)dlsym((void *)-1,"recv");if(!fn)return -1;
    long n=fn(fd,buf,len,flags);int saved=*__errno();
    if(n>0&&(unsigned long)n<=len)station_trace(fd,buf,(unsigned long)n,0);
    *__errno()=saved;return n;
}
long recvfrom(int fd,void *buf,unsigned long len,int flags,void *addr,unsigned *addrlen){
    typedef long(*Fn)(int,void *,unsigned long,int,void *,unsigned *);
    Fn fn=(Fn)dlsym((void *)-1,"recvfrom");if(!fn)return -1;
    long n=fn(fd,buf,len,flags,addr,addrlen);int saved=*__errno();
    if(n>0&&(unsigned long)n<=len)station_trace(fd,buf,(unsigned long)n,0);
    *__errno()=saved;return n;
}
/* Android LP64 sys/socket.h layout, not the 32-bit msghdr ABI. */
struct st_iovec {void *base;unsigned long len;};
struct st_msghdr {void *name;unsigned namelen;struct st_iovec *iov;unsigned long iovlen;void *control;unsigned long controllen;int flags;};
_Static_assert(sizeof(struct st_msghdr)==56,"LP64 msghdr size");
_Static_assert(__builtin_offsetof(struct st_msghdr,iov)==16,"LP64 iov offset");
long recvmsg(int fd,struct st_msghdr *msg,int flags){
    typedef long(*Fn)(int,struct st_msghdr *,int);
    Fn fn=(Fn)dlsym((void *)-1,"recvmsg");if(!fn)return -1;
    long n=fn(fd,msg,flags);int saved=*__errno();
    /* Netlink caller uses one buffer; leave all scatter/gather cases untouched. */
    if(n>0&&msg&&msg->iovlen==1&&msg->iov&&(unsigned long)n<=msg->iov[0].len)
        station_trace(fd,msg->iov[0].base,(unsigned long)n,0);
    *__errno()=saved;return n;
}
