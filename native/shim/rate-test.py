import ctypes, hashlib, json, struct, subprocess, os
from pathlib import Path

p=Path(__file__).resolve().parent
clang=os.environ.get('HOST_CLANG','clang')
subprocess.run([clang,'-x','c','-shared','-fPIC','-nostdlib','-Wall','-Wextra','-Werror',str(p/'rate-compat.h'),'-o',str(p/'rate-host.so')],check=True)
f=ctypes.CDLL(str(p/'rate-host.so')).rewrite_rate
f.argtypes=[ctypes.c_void_p,ctypes.c_uint];f.restype=ctypes.c_int
def attr(t,data):
    out=struct.pack('<HH',len(data)+4,t)+data
    return out+b'\0'*((-len(out))%4)
def u32(v):return struct.pack('<I',v)
def packet(fields,sub=3,oui=0x1a11,extra=b'',unpadded=False):
    if unpadded:
        off=0
        while off+((struct.unpack_from('<H',fields,off)[0]+3)&~3)<len(fields):
            off+=(struct.unpack_from('<H',fields,off)[0]+3)&~3
        fields=fields[:off+struct.unpack_from('<H',fields,off)[0]]
    payload=attr(3,u32(33))+attr(195,u32(oui))+attr(196,u32(sub))+attr(197,fields)+extra
    return struct.pack('<IHHII',20+len(payload),35,5,9,0)+bytes([103,1,0,0])+payload
def rate(pre=2,bw=2,gi=2,nss=2,mcs=3,wide=False):
    return attr(1,u32(pre))+attr(2,u32(bw) if wide else struct.pack('<H',bw))+attr(3,u32(gi))+attr(4,u32(nss) if wide else bytes([nss]))+attr(5,u32(mcs) if wide else bytes([mcs]))
def freq(value=5745,bw=2):return attr(1,u32(value))+attr(2,struct.pack('<H',bw))
cases=[]
def check(name,data,want=0,expected=None):
    buffer=ctypes.create_string_buffer(data,len(data))
    result=f(buffer,len(data))
    assert result==want,(name,result,want)
    assert buffer.raw==(data if expected is None else expected),name
    cases.append(name)

for unpadded in (False,True):
    check('5g-rate-'+str(unpadded),packet(rate(),unpadded=unpadded),2,
          packet(rate(pre=1,bw=0,gi=0,nss=1,mcs=7),unpadded=unpadded))
    check('5745-bandwidth-'+str(unpadded),packet(freq(),sub=1,unpadded=unpadded),3,
          packet(freq(bw=0),sub=1,unpadded=unpadded))
    check('retained-24-'+str(unpadded),packet(rate(pre=1,bw=0,mcs=11),unpadded=unpadded),1,
          packet(rate(pre=1,bw=0,gi=0,nss=1,mcs=0),unpadded=unpadded))
check('retained-24-wide',packet(rate(pre=1,bw=0,mcs=11,wide=True)),1,
      packet(rate(pre=1,bw=0,gi=0,nss=1,mcs=0,wide=True)))

# Exact Stage76 captured rate packet, including an unpadded final nested child.
captured=bytes.fromhex((p/'rate-request.hex').read_text())
expected=bytearray(captured)
expected[52:56]=u32(1);expected[60:62]=b'\0\0';expected[68:72]=u32(0);expected[76]=1;expected[84]=7
check('actual-stage76-rate',captured,2,bytes(expected))
check('5g-wide-not-observed',packet(rate(wide=True)))
for key,value in [('pre',1),('pre',3),('bw',0),('bw',1),('gi',0),('gi',1),('nss',1),('nss',3),('mcs',0),('mcs',4)]:
    check('mismatch-rate-'+key+str(value),packet(rate(**{key:value})))
for value in (2437,5180,5744,5746,5785):check('other-frequency-'+str(value),packet(freq(value),sub=1))
for bw in (0,1,3):check('other-bandwidth-'+str(bw),packet(freq(bw=bw),sub=1))
for fields,sub in [(rate(),3),(freq(),1)]:
    base=packet(fields,sub=sub)
    check('wrong-oui-'+str(sub),packet(fields,sub=sub,oui=7))
    check('wrong-sub-'+str(sub),packet(fields,sub=8))
    check('duplicate-child-'+str(sub),packet(fields+attr(1,u32(2)),sub=sub))
    check('unknown-child-'+str(sub),packet(fields+attr(6,u32(0)),sub=sub))
    check('duplicate-data-'+str(sub),packet(fields,sub=sub,extra=attr(197,fields)))
    check('duplicate-oui-'+str(sub),packet(fields,sub=sub,extra=attr(195,u32(0x1a11))))
    check('missing-child-'+str(sub),packet(fields[8:],sub=sub))
    malformed=bytearray(fields);malformed[0:2]=b'\x03\0'
    check('short-child-'+str(sub),packet(bytes(malformed),sub=sub))
    malformed=bytearray(fields);malformed[0:2]=b'\xff\x7f'
    check('long-child-'+str(sub),packet(bytes(malformed),sub=sub))
    check('tail-garbage-'+str(sub),packet(fields+b'\x01',sub=sub))
    for cut in (0,19,21,len(base)-1):check('truncate-'+str(sub)+'-'+str(cut),base[:cut])
    check('extra-frame-'+str(sub),base+b'\0'*4)
    for offset,value in [(0,0),(4,0),(16,102)]:
        bad=bytearray(base);bad[offset]=value;check('outer-mismatch-'+str(sub)+'-'+str(offset),bytes(bad))
    # Valid header length plus malformed terminal attribute must not write early.
    malformed=bytearray(base+b'\x03\0\x01\0');malformed[:4]=u32(len(malformed))
    check('malformed-tail-'+str(sub),bytes(malformed))
result={'cases':len(cases),'passed':True,'unchanged_nonmatches':True,
        'captured_stage76_rate_verified':True,'frequency_packet':'canonical exact attrs 5745/u32 + bw2/u16; kernel observation verifies values',
        'frequency_preserved_mhz':5745,'real_kernel_ack_preserved':True,
        'cases_run':cases,'header_sha256':hashlib.sha256((p/'rate-compat.h').read_bytes()).hexdigest()}
(p/'rate-test-result.json').write_text(json.dumps(result,indent=2)+'\n')
print('Stage81 MCS7 translator:',len(cases),'cases passed; exact Stage76 capture and unchanged negatives')
