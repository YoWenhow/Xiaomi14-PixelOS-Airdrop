/* SPDX-License-Identifier: GPL-2.0-only
 * Narrow offline-built Wonder test client. No libc, firmware, link-up or retries.
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long usize;
typedef long ssize;
_Static_assert(sizeof(usize) == 8, "64-bit target required");

#ifdef __aarch64__
enum { SC_WRITE=64, SC_CLOSE=57, SC_EXIT=93, SC_SOCKET=198, SC_BIND=200,
       SC_GETSOCKNAME=204, SC_SENDTO=206, SC_RECVFROM=207, SC_PPOLL=73,
       SC_CLOCK=113, SC_IOCTL=29, SC_NANOSLEEP=101, SC_SETSOCKOPT=208, SC_CONNECT=203 };
static long call(long n, long a, long b, long c, long d, long e, long f)
{
	register long x0 __asm__("x0")=a, x1 __asm__("x1")=b, x2 __asm__("x2")=c;
	register long x3 __asm__("x3")=d, x4 __asm__("x4")=e, x5 __asm__("x5")=f;
	register long x8 __asm__("x8")=n;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
			 "r"(x4), "r"(x5), "r"(x8) : "memory", "cc");
	return x0;
}
#else
/* Host-only parser/argument checks use the same code with x86-64 syscalls. */
enum { SC_WRITE=1, SC_CLOSE=3, SC_EXIT=60, SC_SOCKET=41, SC_BIND=49,
       SC_GETSOCKNAME=51, SC_SENDTO=44, SC_RECVFROM=45, SC_PPOLL=271,
       SC_CLOCK=228, SC_IOCTL=16, SC_NANOSLEEP=35, SC_SETSOCKOPT=54, SC_CONNECT=42 };
static long call(long n, long a, long b, long c, long d, long e, long f)
{
	register long r10 __asm__("r10")=d, r8 __asm__("r8")=e, r9 __asm__("r9")=f;
	long ret;
	__asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b),
			 "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx","r11","memory","cc");
	return ret;
}
#endif
void *memcpy(void *d, const void *s, usize n)
{ u8 *a=d; const u8 *b=s; while(n--) *a++=*b++; return d; }
void *memset(void *d, int v, usize n)
{ u8 *p=d; while(n--) *p++=(u8)v; return d; }
static usize len(const char *s) { usize n=0; while(s[n]) n++; return n; }
static int equal(const char *a,const char *b)
{ while(*a && *a==*b) { a++; b++; } return *a==*b; }
static void out(const char *s)
{
	usize n=len(s);
	while(n) { long r=call(SC_WRITE,1,(long)s,n,0,0,0); if(r<=0) break; s+=r; n-=r; }
}
static void number(long v)
{
	char b[24], *p=b+sizeof(b)-1;
	unsigned long x;
	*p=0; if(v<0) { out("-"); x=(unsigned long)(-(v+1))+1; } else x=v;
	do { *--p='0'+x%10; x/=10; } while(x);
	out(p);
}
static __attribute__((noreturn)) void leave(int code)
{ call(SC_EXIT,code,0,0,0,0,0); __builtin_unreachable(); }
static __attribute__((noreturn)) void fail(const char *s,long code)
{ out("ERROR "); out(s); out(" code="); number(code); out("\n"); leave(1); }
static u32 decimal(const char *s,u32 max)
{
	u32 n=0;
	if(!*s) fail("empty integer",-22);
	for(;*s;s++) {
		if(*s<'0'||*s>'9'||n>max/10||(n==max/10&&(u32)(*s-'0')>max%10))
			fail("invalid integer",-22);
		n=n*10+*s-'0';
	}
	return n;
}
static unsigned hex(char c)
{
	if(c>='0'&&c<='9') return c-'0';
	if(c>='a'&&c<='f') return c-'a'+10;
	if(c>='A'&&c<='F') return c-'A'+10;
	fail("invalid IPv6 hex",-22);
}
/* Bounded ICMPv6 diagnostic. Explicit device, source and destination scope.
 * Does not create routes/interfaces, alter sysctls, or send user files. */
struct in6 {u16 family,port;u32 flow;u8 addr[16];u32 scope;};
struct times {long sec,nsec;};
struct pollfd {int fd;short events,revents;};
_Static_assert(sizeof(struct in6)==28,"sockaddr_in6 layout");
static void address(const char *s,u8 b[16]){
    if(len(s)!=32)fail("address must be 32 hex digits",-22);
    for(unsigned i=0;i<16;i++)b[i]=(hex(s[2*i])<<4)|hex(s[2*i+1]);
}
static void result(const char *name,long v){out(name);out("=");number(v);out("\n");}
static void checked(const char *name,long v){result(name,v);if(v<0)leave(1);}
static long millis(void){
    struct times t={0};long r=call(SC_CLOCK,1,(long)&t,0,0,0,0);
    if(r<0)fail("clock",r);return t.sec*1000+t.nsec/1000000;
}
static int app(int argc,char **argv){
    if(argc==2&&equal(argv[1],"--help")){
        out("scope-probe IFACE IFINDEX SOURCE_HEX32 DEST_HEX32 COUNT(1..12)\n");return 0;
    }
    if(argc!=6)fail("expected 5 arguments; see --help",-22);
    usize ilen=len(argv[1]);if(!ilen||ilen>15)fail("interface length",-22);
    for(unsigned i=0;i<ilen;i++){
        char c=argv[1][i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))fail("interface name",-22);
    }
    u32 scope=decimal(argv[2],65535),count=decimal(argv[5],12);
    if(!scope||!count)fail("scope/count cannot be zero",-22);
    struct in6 source={.family=10,.scope=scope},dest={.family=10,.scope=scope};
    address(argv[3],source.addr);address(argv[4],dest.addr);
    long fd=call(SC_SOCKET,10,3,58,0,0,0);checked("socket_raw_icmpv6",fd);
    checked("bind_device",call(SC_SETSOCKOPT,fd,1,25,(long)argv[1],ilen+1,0));
    checked("bind_source_scope",call(SC_BIND,fd,(long)&source,sizeof(source),0,0,0));
    checked("connect_destination_scope",call(SC_CONNECT,fd,(long)&dest,sizeof(dest),0,0,0));
    result("scope_id",scope);
    u8 packet[32]={128,0,0,0,0x50,0x49};
    memcpy(packet+8,"houji-scope-probe",16);
    unsigned sent=0,replies=0;
    for(unsigned seq=1;seq<=count;seq++){
        packet[6]=(u8)(seq>>8);packet[7]=(u8)seq;
        long r=call(SC_SENDTO,fd,(long)packet,sizeof(packet),0,0,0);
        result("send_syscall",r);if(r!=(long)sizeof(packet))break;sent++;
        long deadline=millis()+250;int matched=0;
        while(millis()<deadline){
            long remain=deadline-millis();if(remain<=0)break;
            struct times delay={0,remain*1000000};struct pollfd poll={(int)fd,1,0};
            r=call(SC_PPOLL,(long)&poll,1,(long)&delay,0,0,0);
            if(r<0){result("poll_error",r);break;}if(!r)break;
            u8 data[256];struct in6 from={0};u32 from_len=sizeof(from);
            r=call(SC_RECVFROM,fd,(long)data,sizeof(data),0,(long)&from,(long)&from_len);
            if(r<0){result("recv_error",r);break;}
            if(r<8||data[0]!=129||data[1]!=0||data[4]!=0x50||data[5]!=0x49||data[6]!=(seq>>8)||data[7]!=(seq&255))continue;
            int same=1;for(unsigned i=0;i<16;i++)if(from.addr[i]!=dest.addr[i])same=0;
            if(same&&!matched){matched=1;replies++;result("matched_reply_seq",seq);}
        }
    }
    call(SC_CLOSE,fd,0,0,0,0,0);result("send_success",sent);result("echo_replies",replies);
    return sent==count&&replies==count?0:2;
}
__attribute__((used,noreturn)) void entry(long *stack)
{
	leave(app((int)stack[0],(char **)(stack+1)));
}
#ifdef __aarch64__
__asm__(".global _start\n_start:\nmov x0, sp\nbl entry\n");
#else
__asm__(".global _start\n_start:\nmov %rsp, %rdi\nand $-16, %rsp\ncall entry\n");
#endif
