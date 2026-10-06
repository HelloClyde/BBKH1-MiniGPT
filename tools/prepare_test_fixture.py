"""Synthetic RAM for the host hook state machine; never executable firmware."""
from pathlib import Path
import struct,zlib
ROOT=Path(__file__).resolve().parents[1]
def suffix(data,target):
    base=zlib.crc32(data);basis={}
    for bit in range(32):
        probe=bytearray(data);probe[-4+bit//8]^=1<<(bit%8)
        value=zlib.crc32(probe)^base;mask=1<<bit
        while value:
            pivot=value.bit_length()-1
            if pivot in basis:value^=basis[pivot][0];mask^=basis[pivot][1]
            else:basis[pivot]=(value,mask);break
    value=target^base;mask=0
    while value:
        pivot=value.bit_length()-1;value^=basis[pivot][0];mask^=basis[pivot][1]
    result=bytearray(data);result[-4:]=struct.pack('<I',mask)
    assert zlib.crc32(result)==target;return result

def main():
    data=bytearray(0x50000);base=0x80004000
    struct.pack_into('<II',data,0x8004755c-base,0x3c02b302,0x8c4200a8)
    for addr,n,crc in [(0x80047310,0x36c,0x3d39ee4f),(0x80004978,0x2c,0x2a9148b0),(0x8004b93c,0x70,0x8432e6bb),(0x800486ec,0x40,0xf0abfbbb)]:
        start=addr-base;data[start:start+n]=suffix(data[start:start+n],crc)
    out=ROOT/'build/fixtures/synthetic-ram.bin';out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(data)
    print('Created synthetic host-test RAM, no firmware bytes')
if __name__=='__main__':main()
