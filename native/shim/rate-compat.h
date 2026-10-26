typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
static u16 read16(const u8 *p){return p[0]|((u16)p[1]<<8);}
static u32 read32(const u8 *p){return read16(p)|((u32)read16(p+2)<<16);}
static u8 *nla(u8 *p,unsigned n,unsigned type,unsigned length) {
    u8 *found=0;
    while(n) {
        if(n<4)return 0;
        unsigned size=read16(p),step=(size+3)&~3u;
        if(size<4||size>n)return 0;
        /* NLA payload lengths can omit alignment after their last child. */
        if(step>n){if(size!=n)return 0;step=size;}
        if((read16(p+2)&0x3fff)==type){if(found||size!=length+4)return 0;found=p+4;}
        p+=step;n-=step;
    }
    return found;
}
/* Accept only the exact rate request observed from this donor, not arbitrary
 * unsupported configurations. Packet shape/other requests remain untouched. */
static int rewrite_rate_24(u8 *b,unsigned n) {
    if(n<20||n>4096||read32(b)!=n||read16(b+4)<16||b[16]!=103)return 0;
    u8 *oui=nla(b+20,n-20,195,4),*sub=nla(b+20,n-20,196,4);
    if(!oui||!sub||read32(oui)!=0x1a11||read32(sub)!=3)return 0;
    unsigned off=20;u8 *data=0;unsigned size=0;
    while(off<n){unsigned l=read16(b+off);if(l<4||off+((l+3)&~3u)>n)return 0;
        if((read16(b+off+2)&0x3fff)==197){if(data)return 0;data=b+off+4;size=l-4;}
        off+=(l+3)&~3u;}
    if(!data)return 0;
    u8 *pre=nla(data,size,1,4),*bw=nla(data,size,2,2),*gi=nla(data,size,3,4);
    u8 *nss=nla(data,size,4,1),*mcs=nla(data,size,5,1);
    if(!bw){bw=nla(data,size,2,4);if(bw&&read32(bw)!=0)return 0;}
    if(!nss){nss=nla(data,size,4,4);if(nss&&read32(nss)!=2)return 0;}
    if(!mcs){mcs=nla(data,size,5,4);if(mcs&&read32(mcs)!=11)return 0;}
    if(!pre||!bw||!gi||!nss||!mcs||read32(pre)!=1||read16(bw)!=0||read32(gi)!=2||*nss!=2||*mcs!=11)return 0;
    gi[0]=gi[1]=gi[2]=gi[3]=0;*nss=1;*mcs=0;return 1;
}

/* Only the Stage76 observed 5 GHz tuple. The existing 2.4 GHz translator
 * remains byte-for-byte above, apart from its private function name.
 * Return 2: fixed rate translated; 3: 5745 MHz bandwidth translated.
 * No success response is synthesized: the real kernel ACK is still returned.
 */
static int exact_children(const u8 *p,unsigned n,unsigned count) {
    unsigned seen=0;
    while(n) {
        if(n<4)return 0;
        unsigned size=read16(p),step=(size+3)&~3u,type=read16(p+2)&0x3fff;
        if(size<4||size>n||!type||type>count||(seen&(1u<<type)))return 0;
        if(step>n){if(size!=n)return 0;step=size;}
        seen|=1u<<type;p+=step;n-=step;
    }
    return seen==((1u<<(count+1))-2u);
}
int rewrite_rate(u8 *b,unsigned n) {
    if(rewrite_rate_24(b,n))return 1;
    if(n<20||n>4096||read32(b)!=n||read16(b+4)<16||b[16]!=103)return 0;
    u8 *oui=nla(b+20,n-20,195,4),*sub=nla(b+20,n-20,196,4);
    if(!oui||!sub||read32(oui)!=0x1a11)return 0;
    unsigned command=read32(sub);
    if(command!=1&&command!=3)return 0;
    unsigned off=20;u8 *data=0;unsigned size=0;
    while(off<n){
        if(n-off<4)return 0;
        unsigned len=read16(b+off),step=(len+3)&~3u;
        if(len<4||step>n-off)return 0;
        if((read16(b+off+2)&0x3fff)==197){if(data)return 0;data=b+off+4;size=len-4;}
        off+=step;
    }
    if(!data||!exact_children(data,size,command==1?2:5))return 0;
    u8 *first=nla(data,size,1,4),*bw=nla(data,size,2,2);
    if(!first||!bw||read16(bw)!=2)return 0;
    if(command==1){
        if(read32(first)!=5745)return 0;
        bw[0]=bw[1]=0;
        return 3;
    }
    u8 *gi=nla(data,size,3,4),*nss=nla(data,size,4,1),*mcs=nla(data,size,5,1);
    if(!gi||!nss||!mcs||read32(first)!=2||read32(gi)!=2||*nss!=2||*mcs!=3)return 0;
    first[0]=1;first[1]=first[2]=first[3]=0;
    bw[0]=bw[1]=0;gi[0]=gi[1]=gi[2]=gi[3]=0;*nss=1;*mcs=7;
    return 2;
}
