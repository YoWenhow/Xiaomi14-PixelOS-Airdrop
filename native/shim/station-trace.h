typedef unsigned char s8;
typedef unsigned int s32;
static unsigned st16(const s8 *p){return p[0]|((unsigned)p[1]<<8);}
static s32 st32(const s8 *p){return st16(p)|((s32)st16(p+2)<<16);}
/* Match complete original request, never rewrite it. Validate every outer NLA. */
int station_request(const void *buffer,unsigned length) {
    const s8 *b=buffer;unsigned off=20,oui=0,sub=0,seen=0;
    if(length<20||length>4096||st32(b)!=length||st16(b+4)<16||b[16]!=103)return 0;
    while(off<length){
        if(length-off<4)return 0;
        unsigned n=st16(b+off),type=st16(b+off+2)&0x3fff,step=(n+3)&~3u;
        if(n<4||n>length-off)return 0;
        if(step>length-off){if(n!=length-off)return 0;step=n;}
        if(type==195||type==196){
            unsigned bit=type==195?1:2;
            if(n!=8||(seen&bit))return 0;
            seen|=bit;
            if(type==195)oui=st32(b+off+4);else sub=st32(b+off+4);
        }
        off+=step;
    }
    return seen==3&&oui==0x1a11&&sub==10;
}
