/* SPDX-License-Identifier: GPL-2.0-only
 * Narrow offline-built Wonder test client. No libc, firmware, link-up or retries.
 */
#include "uapi-values.h"
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long usize;
typedef long ssize;
_Static_assert(sizeof(usize) == 8, "64-bit target required");

#ifdef __aarch64__
enum { SC_WRITE=64, SC_CLOSE=57, SC_EXIT=93, SC_SOCKET=198, SC_BIND=200,
       SC_GETSOCKNAME=204, SC_SENDTO=206, SC_RECVFROM=207, SC_PPOLL=73,
       SC_CLOCK=113, SC_IOCTL=29, SC_NANOSLEEP=101 };
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
       SC_CLOCK=228, SC_IOCTL=16, SC_NANOSLEEP=35 };
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
	fail("invalid MAC",-22);
}
static void mac(const char *s,u8 dst[6])
{
	int i,any=0;
	if(len(s)!=17) fail("MAC must contain six octets",-22);
	for(i=0;i<6;i++) {
		if(i&&s[i*3-1]!=':') fail("invalid MAC separator",-22);
		dst[i]=(hex(s[i*3])<<4)|hex(s[i*3+1]); any|=dst[i];
	}
	if(!any||(dst[0]&1)) fail("BSSID must be nonzero unicast",-22);
}
struct nh { u32 size; u16 type,flags; u32 seq,pid; };
struct gh { u8 cmd,version; u16 reserved; };
struct ah { u16 size,type; };
struct addr { u16 family,pad; u32 pid,groups; };
struct times { long sec,nsec; };
struct pollfd { int fd; short events,revents; };
_Static_assert(sizeof(struct nh)==16 && sizeof(struct addr)==12, "netlink ABI");
static u8 tx[2048] __attribute__((aligned(8))), rx[65536] __attribute__((aligned(8)));
static usize used;
static int sock=-1;
static u32 sequence,port;
static u16 family;
static u32 want_phy,want_if;
static int got_family,got_phy,got_if,show_channels;
static u32 observed_freq,observed_width;
static u16 rd16(const void *p) { u16 x; memcpy(&x,p,2); return x; }
static u32 rd32(const void *p) { u32 x; memcpy(&x,p,4); return x; }
static usize align4(usize n) { return (n+3)&~(usize)3; }
static int attr_valid(const u8 *p,usize left)
{
	if(left<4) return 0;
	u16 n=rd16(p);
	return n>=4 && n<=left && align4(n)<=left;
}
static int attr_stream(const u8 *p,usize n)
{
	while(n) { if(!attr_valid(p,n)) return 0; usize k=align4(rd16(p)); p+=k; n-=k; }
	return 1;
}
static const u8 *find(const u8 *p,usize n,u16 key,usize *size)
{
	const u8 *found=0;
	if(!attr_stream(p,n)) fail("malformed attributes",-74);
	while(n) {
		u16 k=rd16(p), type=rd16(p+2)&0x3fff;
		if(type==key) {
			if(found) fail("duplicate scalar attribute",-74);
			found=p+4; *size=k-4;
		}
		usize step=align4(k); p+=step; n-=step;
	}
	return found;
}
static int str_attr(const u8 *p,usize n,const char *expected)
{ return p && n==len(expected)+1 && p[n-1]==0 && equal((const char *)p,expected); }
static u32 get32(const u8 *p,usize n,u16 key)
{
	usize k=0; const u8 *v=find(p,n,key,&k);
	if(!v||k!=4) fail("missing u32 attribute",key);
	return rd32(v);
}
static void begin(u16 type,u8 cmd,int dump)
{
	memset(tx,0,sizeof(tx)); used=sizeof(struct nh)+sizeof(struct gh);
	struct nh *h=(void *)tx; struct gh *g=(void *)(tx+sizeof(*h));
	h->type=type; h->flags=1|4|(dump?0x300:0); h->seq=++sequence; h->pid=port;
	g->cmd=cmd; g->version=1;
}
static void attr(u16 type,const void *p,usize n)
{
	usize total=align4(n+4);
	if(n>65531||used+total>sizeof(tx)) fail("request too large",-90);
	struct ah h={n+4,type};
	memcpy(tx+used,&h,4); if(n) memcpy(tx+used+4,p,n);
	memset(tx+used+4+n,0,total-4-n); used+=total;
}
static void a32(u16 t,u32 v) { attr(t,&v,4); }
static void a16(u16 t,u16 v) { attr(t,&v,2); }
static void a8(u16 t,u8 v) { attr(t,&v,1); }
static long millis(void)
{
	struct times t; long r=call(SC_CLOCK,1,(long)&t,0,0,0,0);
	if(r<0) fail("clock_gettime",r);
	return t.sec*1000+t.nsec/1000000;
}
static int ack_parse(const u8 *p,usize n,u16 type,u32 seq,int *error)
{
	struct nh orig;
	if(n<36) return 0;
	memcpy(&orig,p+20,16);
	if(orig.seq!=seq||orig.type!=type) return 0;
	*error=(int)rd32(p+16);
	return *error<=0 && *error>=-4095;
}
static void frequencies(const u8 *p,usize n)
{
	if(!attr_stream(p,n)) fail("bad bands",-74);
	while(n) {
		usize bn=rd16(p)-4,sz=0; const u8 *f=find(p+4,bn,NL80211_BAND_ATTR_FREQS,&sz);
		if(f) {
			if(!attr_stream(f,sz)) fail("bad frequency list",-74);
			while(sz) {
				usize fn=rd16(f)-4,k=0; const u8 *v=find(f+4,fn,NL80211_FREQUENCY_ATTR_FREQ,&k);
				if(v&&k==4) {
					out("  freq="); number(rd32(v));
					if(find(f+4,fn,NL80211_FREQUENCY_ATTR_DISABLED,&k)) out(" disabled");
					if(find(f+4,fn,NL80211_FREQUENCY_ATTR_NO_IR,&k)) out(" no_ir");
					if(find(f+4,fn,NL80211_FREQUENCY_ATTR_RADAR,&k)) out(" radar");
					out("\n");
				}
				usize step=align4(rd16(f)); f+=step; sz-=step;
			}
		}
		usize step=align4(rd16(p)); p+=step; n-=step;
	}
}
enum answer { NONE,FAMILY,LIST,PHY,IFACE,REG };
static void answer(enum answer mode,const u8 *p,usize n)
{
	usize sz=0; const u8 *v;
	if(mode==FAMILY) {
		v=find(p,n,CTRL_ATTR_FAMILY_NAME,&sz);
		if(!str_attr(v,sz,"nl80211")) fail("unexpected generic-netlink family",-74);
		v=find(p,n,CTRL_ATTR_FAMILY_ID,&sz);
		if(!v||sz!=2||rd16(v)<16) fail("invalid family ID",-74);
		family=rd16(v); got_family=1; return;
	}
	if(mode==LIST||mode==PHY) {
		u32 phy=get32(p,n,NL80211_ATTR_WIPHY);
		v=find(p,n,NL80211_ATTR_WIPHY_NAME,&sz);
		if(mode==PHY) {
			if(phy!=want_phy||!str_attr(v,sz,"wonder")) fail("target is not Wonder wiphy",-19);
			got_phy=1;
		}
		if(mode==LIST||show_channels) {
			out("phy="); number(phy);
			if(v) {
				if(!sz||v[sz-1]||len((const char *)v)!=sz-1) fail("invalid wiphy name",-74);
				out(" name="); out((const char *)v);
			}
			out("\n");
		}
		if(show_channels) { v=find(p,n,NL80211_ATTR_WIPHY_BANDS,&sz); if(v) frequencies(v,sz); }
		return;
	}
	if(mode==IFACE) {
		if(get32(p,n,NL80211_ATTR_IFINDEX)!=want_if ||
		   get32(p,n,NL80211_ATTR_WIPHY)!=want_phy ||
		   get32(p,n,NL80211_ATTR_IFTYPE)!=NL80211_IFTYPE_MONITOR)
			fail("interface identity mismatch",-19);
		v=find(p,n,NL80211_ATTR_IFNAME,&sz);
		if(!str_attr(v,sz,"wonder0")) fail("expected wonder0",-19);
		got_if=1;
        v=find(p,n,NL80211_ATTR_WIPHY_FREQ,&sz);
        observed_freq=(v&&sz==4)?rd32(v):0;
        v=find(p,n,NL80211_ATTR_CHANNEL_WIDTH,&sz);
        observed_width=(v&&sz==4)?rd32(v):0xffffffff;
        out("software_freq=");number(observed_freq);out(" width=");number(observed_width);out("\n");
        return;
	}
	if(mode==REG) {
		v=find(p,n,NL80211_ATTR_REG_ALPHA2,&sz);
		if(!v||sz<2||sz>3) fail("invalid regulatory reply",-74);
		char country[3]={(char)v[0],(char)v[1],0};
		out("reg_alpha2="); out(country); out("\n");
	}
}
static void request(enum answer mode,int dump)
{
	struct nh *sent=(void *)tx;
	struct addr kernel={16,0,0,0};
	u8 cmd=((struct gh *)(tx+16))->cmd;
	int ack=0,done=0,reply=0;
	sent->size=used;
	long r=call(SC_SENDTO,sock,(long)tx,used,0,(long)&kernel,sizeof(kernel));
	if(r!=(long)used) fail("sendto",r);
	long end=millis()+5000;
	while(!(dump?done:(ack&&(mode==NONE||reply)))) {
		long left=end-millis(); if(left<=0) fail("netlink ACK timeout",-110);
		struct times t={left/1000,(left%1000)*1000000};
		struct pollfd f={sock,1,0};
		r=call(SC_PPOLL,(long)&f,1,(long)&t,0,0,0);
		if(r==-4) continue;
		if(r<=0) fail("netlink poll",r?r:-110);
		if(f.revents&~1) fail("netlink poll error",f.revents);
		struct addr from={0}; u32 addrlen=sizeof(from);
		r=call(SC_RECVFROM,sock,(long)rx,sizeof(rx),0x20,(long)&from,(long)&addrlen);
		if(r==-4) continue;
		if(r<=0||(usize)r>sizeof(rx)||addrlen!=sizeof(from)||
		   from.family!=16||from.pid||from.groups) fail("invalid netlink sender/size",r);
		usize remain=r; const u8 *p=rx;
		while(remain) {
			if(remain<16) fail("short netlink header",-74);
			struct nh h; memcpy(&h,p,16);
			if(h.size<16||h.size>remain||align4(h.size)>remain) fail("bad netlink size",-74);
			if(h.seq==sequence) {
				if(h.flags&0x10) fail("interrupted netlink dump",-4);
				if(h.type==2) {
					int error=0;
					if(!ack_parse(p,h.size,sent->type,sequence,&error))
						fail("short/unrelated/invalid ACK",-74);
					if(error) fail("kernel ACK",error);
					ack=1;
				} else if(h.type==3) {
					if(!dump||h.size<20) fail("unexpected DONE",-74);
					int error=(int)rd32(p+16);
					if(error) fail("dump error",error);
					done=1;
				} else if(h.type==sent->type) {
					if(h.size<20) fail("short genl reply",-74);
					u8 rc=p[16];
					if((mode==FAMILY&&rc!=CTRL_CMD_NEWFAMILY)||
					   ((mode==LIST||mode==PHY)&&rc!=NL80211_CMD_NEW_WIPHY)||
					   (mode==IFACE&&rc!=NL80211_CMD_NEW_INTERFACE)||
					   (mode==REG&&rc!=NL80211_CMD_GET_REG))
						fail("unexpected reply command",rc);
					if(mode!=NONE) answer(mode,p+20,h.size-20);
					reply=1;
				} else if(h.type!=1) fail("unexpected netlink type",h.type);
			}
			usize step=align4(h.size); p+=step; remain-=step;
		}
	}
	out(dump?"done=0 seq=":"ack=0 seq="); number(sequence); out(" cmd="); number(cmd); out("\n");
}
static void connect_netlink(void)
{
	sock=call(SC_SOCKET,16,3|0x80000,16,0,0,0);
	if(sock<0) fail("netlink socket",sock);
	struct addr local={16,0,0,0}; u32 size=sizeof(local);
	long r=call(SC_BIND,sock,(long)&local,sizeof(local),0,0,0);
	if(r<0) fail("netlink bind",r);
	r=call(SC_GETSOCKNAME,sock,(long)&local,(long)&size,0,0,0);
	if(r<0||size!=sizeof(local)||!local.pid) fail("getsockname",r);
	port=local.pid;
	begin(16,CTRL_CMD_GETFAMILY,0); attr(CTRL_ATTR_FAMILY_NAME,"nl80211",8);
	request(FAMILY,0); if(!got_family) fail("family missing",-2);
}
static void verify_phy(void)
{
	begin(family,NL80211_CMD_GET_WIPHY,0); a32(NL80211_ATTR_WIPHY,want_phy);
	request(PHY,0); if(!got_phy) fail("wiphy missing",-19);
}
static void verify_interface(int up)
{
	begin(family,NL80211_CMD_GET_INTERFACE,0); a32(NL80211_ATTR_IFINDEX,want_if);
	request(IFACE,0); if(!got_if) fail("interface missing",-19);
	struct { char name[16]; u8 data[24]; } ifr={0};
	memcpy(ifr.name,"wonder0",8);
	long fd=call(SC_SOCKET,2,2|0x80000,0,0,0,0);
	if(fd<0) fail("ioctl socket",fd);
	long r=call(SC_IOCTL,fd,0x8913,(long)&ifr,0,0,0);
	call(SC_CLOSE,fd,0,0,0,0,0);
	if(r<0) fail("SIOCGIFFLAGS",r);
	if(!!(rd16(ifr.data)&1)!=up) fail(up?"frequency requires UP":"cache requires DOWN",-16);
}
static usize vendor(u32 sub)
{
	begin(family,NL80211_CMD_VENDOR,0);
	a32(NL80211_ATTR_IFINDEX,want_if); a32(NL80211_ATTR_VENDOR_ID,0x001a11);
	a32(NL80211_ATTR_VENDOR_SUBCMD,sub);
	usize at=used; attr(NL80211_ATTR_VENDOR_DATA,0,0); return at;
}
static void finish_vendor(usize at)
{ ((struct ah *)(tx+at))->size=used-at; request(NONE,0); }
static void frequency(u32 freq)
{ usize at=vendor(1); a32(1,freq); a16(2,0); finish_vendor(at); }
static void help(void)
{
	out("Wonder probe (no link-up or retries)\n"
	    "  list | reg [phy] | wiphy <phy> | create <phy>\n"
	    "  cache <phy> <ifindex> <MHz> <CC> <unicast-BSSID>\n"
	    "  freq <phy> <ifindex> <MHz>\n"
	    "Writes require wiphy=wonder and interface=wonder0, type passive monitor.\n");
}
static int base_app(int argc,char **argv)
{
	enum { LIST_OP,REG_OP,WIPHY_OP,CREATE_OP,CACHE_OP,FREQ_OP } op;
	u32 freq=0; u8 bssid[6]={0}; const char *country=0;
	if(argc==2&&equal(argv[1],"--help")) { help(); return 0; }
	if(argc==2&&equal(argv[1],"list")) op=LIST_OP;
	else if((argc==2||argc==3)&&equal(argv[1],"reg")) op=REG_OP;
	else if(argc==3&&equal(argv[1],"wiphy")) op=WIPHY_OP;
	else if(argc==3&&equal(argv[1],"create")) op=CREATE_OP;
	else if(argc==7&&equal(argv[1],"cache")) op=CACHE_OP;
	else if(argc==5&&equal(argv[1],"freq")) op=FREQ_OP;
	else { help(); return 2; }
	if(op>=WIPHY_OP) want_phy=decimal(argv[2],0x7fffffff);
	if(op==REG_OP&&argc==3) want_phy=decimal(argv[2],0x7fffffff);
	if(op>=CACHE_OP) {
		want_if=decimal(argv[3],0x7fffffff); freq=decimal(argv[4],7125);
		if(!want_if||freq<2300) fail("invalid ifindex/frequency",-22);
	}
	if(op==CACHE_OP) {
		country=argv[5];
		if(len(country)!=2||!((country[0]>='A'&&country[0]<='Z'&&country[1]>='A'&&country[1]<='Z')||
				    equal(country,"00"))) fail("country requires AA or 00",-22);
		mac(argv[6],bssid);
	}
	connect_netlink();
	if(op==LIST_OP) {
		begin(family,NL80211_CMD_GET_WIPHY,1); attr(NL80211_ATTR_SPLIT_WIPHY_DUMP,0,0); request(LIST,1);
	} else if(op==REG_OP) {
		/* Read-only: permit the physical QCA wiphy as well as Wonder. */
		begin(family,NL80211_CMD_GET_REG,0);
		if(argc==3) a32(NL80211_ATTR_WIPHY,want_phy);
		request(REG,0);
	} else {
		show_channels=op==WIPHY_OP; verify_phy();
		if(op==CREATE_OP) {
			begin(family,NL80211_CMD_NEW_INTERFACE,0); a32(NL80211_ATTR_WIPHY,want_phy);
			attr(NL80211_ATTR_IFNAME,"wonder0",8); a32(NL80211_ATTR_IFTYPE,NL80211_IFTYPE_MONITOR);
			request(NONE,0);
		} else if(op>=CACHE_OP) {
			verify_interface(op==FREQ_OP);
			frequency(freq);
			if(op==CACHE_OP) {
				usize at=vendor(2); a32(1,0);
				usize nested=used; attr(2|0x8000,0,0); a8(1,1); attr(2,bssid,6);
				((struct ah *)(tx+nested))->size=used-nested; finish_vendor(at);
				at=vendor(3); a32(1,1); a16(2,0); a32(3,0); a8(4,1); a8(5,0); finish_vendor(at);
				at=vendor(4); attr(1,country,3); finish_vendor(at);
			}
		}
	}
	call(SC_CLOSE,sock,0,0,0,0,0); return 0;
}

/* One passive monitor named wonder0 satisfies Wonder's RX device lookup.
 * No station is opened. All transmissions are explicit and bounded.
 */
static void software_channel(u32 freq)
{
    begin(family,NL80211_CMD_SET_WIPHY,0);
    a32(NL80211_ATTR_IFINDEX,want_if);
    a32(NL80211_ATTR_WIPHY_FREQ,freq);
    a32(NL80211_ATTR_CENTER_FREQ1,freq);
    a32(NL80211_ATTR_CHANNEL_WIDTH,NL80211_CHAN_WIDTH_20);
    request(NONE,0);
    got_if=0;verify_interface(1);
    if(observed_freq!=freq||observed_width!=NL80211_CHAN_WIDTH_20)
        fail("software channel readback mismatch",-5);
}
static void transmit_test(const u8 source[6])
{
    /* Ordinary directed probe requests for a nonexistent test SSID.
     * No Apple identity, deauthentication, user file or unicast peer.
     */
    u8 packet[52]={0};packet[2]=8;packet[8]=0x40;
    memset(packet+12,0xff,6);memcpy(packet+18,source,6);memset(packet+24,0xff,6);
    packet[32]=0;packet[33]=13;memcpy(packet+34,"houji-tx-test",13);
    packet[47]=1;packet[48]=3;packet[49]=0x8c;packet[50]=0x98;packet[51]=0xb0;
    struct { u16 family,protocol; int index; u16 hatype; u8 pkttype,halen,addr[8]; }
        ll={17,0x0300,(int)want_if,0,0,0,{0}};
    _Static_assert(sizeof(ll)==20,"sockaddr_ll ABI");
    long fd=call(SC_SOCKET,17,3|0x80000|0x800,0x0300,0,0,0);
    if(fd<0)fail("packet socket",fd);
    long r=call(SC_BIND,fd,(long)&ll,sizeof(ll),0,0,0);
    if(r<0)fail("packet bind",r);
    for(int i=1;i<=3;i++) {
        packet[30]=(u8)(i<<4);
        r=call(SC_SENDTO,fd,(long)packet,sizeof(packet),0,0,0);
        if(r!=sizeof(packet))fail("packet send",r);
    }
    call(SC_CLOSE,fd,0,0,0,0,0);
    out("submitted=3 bytes_each=52; socket acceptance is NOT RF/ACK evidence\n");
}
/* Independent receiver test: fixed unique SSID, 12 seconds, no association service. */
static usize beacon_packet(u8 *p, const u8 source[6], unsigned sequence, unsigned long tsf)
{
    memset(p,0,128);p[2]=8;p[8]=0x80;
    memset(p+12,0xff,6);memcpy(p+18,source,6);memcpy(p+24,source,6);
    p[30]=(u8)(sequence<<4);p[31]=(u8)(sequence>>4);
    for(int i=0;i<8;i++)p[32+i]=(u8)(tsf>>(8*i));
    p[40]=50;p[42]=1;p[43]=4; /* 50 TU, ESS + short slot; no privacy */
    const char name[]="houji-s36-7c92";
    usize n=44;p[n++]=0;p[n++]=sizeof(name)-1;
    memcpy(p+n,name,sizeof(name)-1);n+=sizeof(name)-1;
    const u8 ies[]={1,8,0x8c,0x12,0x98,0x24,0xb0,0x48,0x60,0x6c,
                   3,1,36,5,4,0,1,0,0};
    memcpy(p+n,ies,sizeof(ies));return n+sizeof(ies);
}
static void beacon_test(const u8 source[6])
{
    struct { u16 family,protocol; int index; u16 hatype; u8 pkttype,halen,addr[8]; }
        ll={17,0x0300,(int)want_if,0,0,0,{0}};
    long fd=call(SC_SOCKET,17,3|0x80000|0x800,0x0300,0,0,0);
    if(fd<0)fail("packet socket",fd);
    long r=call(SC_BIND,fd,(long)&ll,sizeof(ll),0,0,0);
    if(r<0)fail("packet bind",r);
    long end=millis()+12000;unsigned sent=0;
    while(sent<240&&millis()<end) {
        u8 p[128];usize n=beacon_packet(p,source,sent+1,(unsigned long)millis()*1000);
        r=call(SC_SENDTO,fd,(long)p,n,0,0,0);
        if(r!=(long)n)fail("beacon send",r);
        sent++;
        long delay[2]={0,50000000};
        r=call(SC_NANOSLEEP,(long)delay,0,0,0,0,0);
        if(r<0)fail("beacon delay",r);
    }
    call(SC_CLOSE,fd,0,0,0,0,0);
    out("beacon_submitted=");number(sent);out(" ssid=houji-s36-7c92 freq=5180; receiver evidence required\n");
}

static int app(int argc,char **argv)
{
    if(argc==2&&equal(argv[1],"beacon-dump")) {
        u8 packet[128],source[6]={2,0x73,0x36,0x7c,0x92,1};
        usize n=beacon_packet(packet,source,1,12345678);
        call(SC_WRITE,1,(long)packet,n,0,0,0);return 0;
    }
    int beacon=argc==6&&equal(argv[1],"beacon");
    int channel=argc==5&&equal(argv[1],"channel");
    int info=argc==4&&equal(argv[1],"info");
    int send=argc==6&&equal(argv[1],"tx");
    if(!channel&&!info&&!send&&!beacon)return base_app(argc,argv);
    want_phy=decimal(argv[2],0x7fffffff);want_if=decimal(argv[3],0x7fffffff);
    if(!want_if)fail("invalid interface index",-22);
    u32 freq=0;u8 source[6]={0};
    if(!info){freq=decimal(argv[4],7125);if(freq!=2437&&freq!=5745&&freq!=5180)fail("unsupported test channel",-22);}
    if(send||beacon)mac(argv[5],source);
    if(beacon&&freq!=5180)fail("beacon only on test channel 36",-22);
    connect_netlink();verify_phy();verify_interface(1);
    if(channel)software_channel(freq);
    if(send||beacon) {
        if(observed_freq!=freq||observed_width!=NL80211_CHAN_WIDTH_20)
            fail("TX requires matching software channel",-16);
        if(beacon)beacon_test(source);else transmit_test(source);
    }
    call(SC_CLOSE,sock,0,0,0,0,0);return 0;
}

#ifdef SELFTEST
static int selftest(void)
{
	u8 test[8]={4,0,1,0,4,0,2,0}, bssid[6]; usize n=0;
	if(!attr_stream(test,8)||attr_stream(test,7)||attr_stream(test+1,7)) return 10;
	if(!find(test,8,2,&n)||n||find(test,8,3,&n)) return 11;
	test[0]=3; if(attr_stream(test,8)) return 12;
	test[0]=9; if(attr_stream(test,8)) return 13;
	test[0]=5; if(!attr_stream(test,8)) return 14;
	if(decimal("2147483647",0x7fffffff)!=0x7fffffff) return 15;
	mac("02:00:00:00:00:01",bssid); if(bssid[0]!=2||bssid[5]!=1) return 16;
	begin(42,NL80211_CMD_VENDOR,0); usize at=used; attr(2|0x8000,0,0);
	a16(2,0); ((struct ah *)(tx+at))->size=used-at;
	if(!attr_stream(tx+20,used-20)||used!=32||rd32(tx+28)!=0) return 17;
	if(!str_attr((const u8 *)"wonder",7,"wonder")||str_attr((const u8 *)"wonder",6,"wonder")) return 18;
	u8 ack[36]={0}; struct nh orig={20,42,1,9,0}; int error=1;
	memcpy(ack+20,&orig,16);
	if(!ack_parse(ack,36,42,9,&error)||error) return 19;
	if(ack_parse(ack,35,42,9,&error)||ack_parse(ack,36,43,9,&error)||
	   ack_parse(ack,36,42,10,&error)) return 20;
	int denied=-16; memcpy(ack+16,&denied,4);
	if(!ack_parse(ack,36,42,9,&error)||error!=-16) return 21;
	denied=1; memcpy(ack+16,&denied,4); if(ack_parse(ack,36,42,9,&error)) return 22;
	out("SELFTEST PASS: ACK error/sequence/type/length, attributes, padding, inputs and identity\n"); return 0;
}
#endif
__attribute__((used,noreturn)) void entry(long *stack)
{
#ifdef SELFTEST
	(void)stack; leave(selftest());
#else
	leave(app((int)stack[0],(char **)(stack+1)));
#endif
}
#ifdef __aarch64__
__asm__(".global _start\n_start:\nmov x0, sp\nbl entry\n");
#else
__asm__(".global _start\n_start:\nmov %rsp, %rdi\nand $-16, %rsp\ncall entry\n");
#endif
