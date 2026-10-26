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
