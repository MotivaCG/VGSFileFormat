"""Remove two LLVM-only feature annotations unknown to old Binaryen.

Does not change instructions or required runtime features. Work on a copy of the
static archive and rebuild its symbol index with the Emscripten llvm-ar.
"""
from pathlib import Path
import subprocess,sys
def read_vi(b,at):
    n=0;shift=0
    while True:
        c=b[at];at+=1;n|=(c&127)<<shift
        if c<128:return n,at
        shift+=7
def vi(n):
    b=bytearray()
    while n>=128:b.append((n&127)|128);n>>=7
    b.append(n);return bytes(b)
def string(b,at):
    n,at=read_vi(b,at);return b[at:at+n],at+n
def wasm(b):
    if not b.startswith(b'\0asm'):return b,0
    out=bytearray(b[:8]);at=8;removed=0
    while at<len(b):
        section=b[at];at+=1;size,at=read_vi(b,at);payload=b[at:at+size];at+=size
        if section==0:
            name,pos=string(payload,0)
            if name==b'target_features':
                count,pos=read_vi(payload,pos);features=[]
                for _ in range(count):
                    prefix=payload[pos];pos+=1;feature,pos=string(payload,pos)
                    if feature in (b'bulk-memory-opt',b'call-indirect-overlong'):removed+=1
                    else:features.append(bytes([prefix])+vi(len(feature))+feature)
                assert pos==len(payload)
                payload=vi(len(name))+name+vi(len(features))+b''.join(features)
        out+=bytes([section])+vi(len(payload))+payload
    return bytes(out),removed
src,dest,ar=map(Path,sys.argv[1:4]);b=src.read_bytes();assert b[:8]==b'!<arch>\n'
out=bytearray(b[:8]);at=8;count=0
while at<len(b):
    header=b[at:at+60];at+=60;assert header[-2:]==b'`\n';n=int(header[48:58]);payload=b[at:at+n];at+=n+(n&1)
    if header[:16].strip()==b'/':continue
    payload,changed=wasm(payload);count+=changed
    out+=header[:48]+str(len(payload)).encode().ljust(10,b' ')+header[58:]+payload
    if len(payload)&1:out+=b'\n'
dest.write_bytes(out);subprocess.run([str(ar),'s',str(dest)],check=True)
print(f'{count} non-runtime feature annotations removed; instructions unchanged')
