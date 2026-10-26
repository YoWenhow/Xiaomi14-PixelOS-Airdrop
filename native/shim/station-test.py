import ctypes,struct,subprocess,json,os
from pathlib import Path
p=Path(__file__).resolve().parent
clang=os.environ.get('HOST_CLANG','clang')
subprocess.run([clang,'-x','c','-shared','-fPIC','-nostdlib','-Wall','-Wextra','-Werror',str(p/'station-trace.h'),'-o',str(p/'station-host.so')],check=True)
lib=ctypes.CDLL(str(p/'station-host.so'));f=lib.station_request
f.argtypes=[ctypes.c_void_p,ctypes.c_uint];f.restype=ctypes.c_int
def attr(t,b):
    r=struct.pack('<HH',len(b)+4,t)+b;return r+b'\0'*((-len(r))%4)
def packet(oui=0x1a11,sub=10,action=1,duplicate=False):
    a=attr(3,struct.pack('<I',35))+attr(195,struct.pack('<I',oui))+attr(196,struct.pack('<I',sub))
    if duplicate:a+=attr(196,struct.pack('<I',sub))
    a+=attr(197,attr(1,struct.pack('<I',action))+attr(2,b'\x02\x00\x00\x00\x00\x01'))
    return struct.pack('<IHHII',20+len(a),39,5,14,0)+bytes([103,1,0,0])+a
valid=packet();bad_nla=bytearray(valid);bad_nla[20:22]=b'\xff\xff'
cases=[(packet(action=i),1) for i in range(5)]+[(packet(oui=7),0),(packet(sub=3),0),(packet(duplicate=True),0),(valid[:-1],0),(valid+b'\0',0),(bytes(bad_nla),0),(b'',0)]
for b,want in cases:
    out=ctypes.create_string_buffer(b);assert f(out,len(b))==want;assert out.raw[:len(b)]==b
(p/'station-test-result.json').write_text(json.dumps({'cases':len(cases),'passed':True,'input_unchanged':True})+'\n')
print('Station request selector: 12 cases passed; input untouched')
