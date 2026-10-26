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
       SC_CLOCK=113, SC_IOCTL=29, SC_NANOSLEEP=101, SC_SETSOCKOPT=208, SC_CONNECT=203, SC_GETSOCKOPT=209 };
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
       SC_CLOCK=228, SC_IOCTL=16, SC_NANOSLEEP=35, SC_SETSOCKOPT=54, SC_CONNECT=42, SC_GETSOCKOPT=55 };
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
/* Narrow discovery/connect probe; never uploads files or sends HTTP requests. */
static void dump_hex(const u8 *b,usize n){
    const char *digits="0123456789abcdef";char chunk[129];
    out("dns_response_hex=");
    for(usize pos=0;pos<n;){usize count=n-pos;if(count>64)count=64;
        for(usize i=0;i<count;i++){chunk[2*i]=digits[b[pos+i]>>4];chunk[2*i+1]=digits[b[pos+i]&15];}
        chunk[2*count]=0;out(chunk);pos+=count;}
    out("\n");
}
static int app(int argc,char **argv){
    if(argc==2&&equal(argv[1],"--help")){out("service-probe dns|mdns|tcp IFACE IFINDEX SOURCE_HEX32 DEST_HEX32 PORT\n");return 0;}
    if(argc!=7)fail("expected mode and 5 parameters",-22);
    int mdns=equal(argv[1],"mdns"),dns=equal(argv[1],"dns")||mdns;if(!dns&&!equal(argv[1],"tcp"))fail("mode",-22);
    usize ilen=len(argv[2]);if(!ilen||ilen>15)fail("interface length",-22);
    u32 scope=decimal(argv[3],65535),port=decimal(argv[6],65535);if(!scope||!port)fail("zero scope/port",-22);
    if(mdns&&port!=5353)fail("mdns port must be 5353",-22);
    struct in6 source={.family=10,.scope=scope},dest={.family=10,.scope=scope,.port=(u16)((port<<8)|(port>>8))};
    address(argv[4],source.addr);address(argv[5],dest.addr);
    long fd=call(SC_SOCKET,10,dns?2:(1|2048),dns?17:6,0,0,0);checked("socket",fd);
    if(mdns){
        int one=1;checked("reuse_address",call(SC_SETSOCKOPT,fd,1,2,(long)&one,sizeof(one),0));
        for(unsigned i=0;i<16;i++)source.addr[i]=0;
        source.port=dest.port; /* Bind ::5353; device binding selects the AWDL interface. */
    }
    checked("bind_device",call(SC_SETSOCKOPT,fd,1,25,(long)argv[2],ilen+1,0));
    checked("bind_source_scope",call(SC_BIND,fd,(long)&source,sizeof(source),0,0,0));
    int hops=255;checked("unicast_hops",call(SC_SETSOCKOPT,fd,41,16,(long)&hops,sizeof(hops),0));
    struct in6 group={.family=10,.port=dest.port,.scope=scope};
    long r=0;
    if(mdns){
        struct {u8 addr[16];u32 interface;} membership={.interface=scope};
        _Static_assert(sizeof(membership)==20,"ipv6_mreq layout");
        group.addr[0]=0xff;group.addr[1]=2;group.addr[15]=0xfb;
        memcpy(membership.addr,group.addr,16);
        checked("multicast_interface",call(SC_SETSOCKOPT,fd,41,17,(long)&scope,sizeof(scope),0));
        checked("multicast_hops",call(SC_SETSOCKOPT,fd,41,18,(long)&hops,sizeof(hops),0));
        checked("join_multicast",call(SC_SETSOCKOPT,fd,41,20,(long)&membership,sizeof(membership),0));
    }else{
        r=call(SC_CONNECT,fd,(long)&dest,sizeof(dest),0,0,0);result("connect",r);
    }
    if(!dns){
        if(r==-115){struct pollfd wait={(int)fd,4,0};struct times timeout={3,0};
            r=call(SC_PPOLL,(long)&wait,1,(long)&timeout,0,0,0);result("connect_poll",r);
            if(r>0){int error=-1;u32 n=sizeof(error);r=call(SC_GETSOCKOPT,fd,1,4,(long)&error,(long)&n,0);checked("get_socket_error",r);r=-error;}
            else r=-110;
        }
        result("tcp_connect_result",r);call(SC_CLOSE,fd,0,0,0,0,0);return r?2:0;
    }
    if(r<0)leave(1);
    static const u8 dns_query[]={0x51,0x52,0,0,0,1,0,0,0,0,0,0,
        8,'_','a','i','r','d','r','o','p',4,'_','t','c','p',5,'l','o','c','a','l',0,0,12,0,1};
    static const u8 mdns_query[]={0,0,0,0,0,1,0,0,0,0,0,0,
        8,'_','a','i','r','d','r','o','p',4,'_','t','c','p',5,'l','o','c','a','l',0,0,12,0x80,1};
    _Static_assert(sizeof(dns_query)==sizeof(mdns_query),"DNS query layouts");
    const u8 *query=mdns?mdns_query:dns_query;
    unsigned sends=0,responses=0;long begin=millis(),next=begin,deadline=begin+6000;
    while(millis()<deadline){
        long now=millis();
        if(now>=next&&sends<3){r=call(SC_SENDTO,fd,(long)query,sizeof(dns_query),0,mdns?(long)&group:0,mdns?sizeof(group):0);result("dns_send",r);sends++;next=begin+sends*2000;}
        long until=next<deadline?next:deadline;long remain=until-millis();if(remain<=0)continue;
        struct times timeout={remain/1000,(remain%1000)*1000000};struct pollfd wait={(int)fd,1,0};
        r=call(SC_PPOLL,(long)&wait,1,(long)&timeout,0,0,0);if(r<0){result("poll_error",r);break;}if(!r)continue;
        u8 buf[4096];struct in6 from={0};u32 from_len=sizeof(from);
        r=call(SC_RECVFROM,fd,(long)buf,sizeof(buf),mdns?32:0,mdns?(long)&from:0,mdns?(long)&from_len:0);result("dns_receive",r);
        if(r<0)break;
        if(r>(long)sizeof(buf))continue;
        if(mdns){
            if(from_len!=sizeof(from)||from.family!=10||from.port!=dest.port||(from.scope&&from.scope!=scope))continue;
            int same=1;for(unsigned i=0;i<16;i++)if(from.addr[i]!=dest.addr[i])same=0;
            if(!same)continue;
        }
        if(r>=12&&buf[0]==(mdns?0:0x51)&&buf[1]==(mdns?0:0x52)&&(buf[2]&0x80)){
            dump_hex(buf,(usize)r);responses++;
            if(!mdns){call(SC_CLOSE,fd,0,0,0,0,0);return 0;}
        }
    }
    call(SC_CLOSE,fd,0,0,0,0,0);return responses?0:2;
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
