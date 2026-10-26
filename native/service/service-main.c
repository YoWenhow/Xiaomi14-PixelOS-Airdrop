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
