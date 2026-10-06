from build_minigpt import toolchain_tool
"""Actual MIPS/MXU leaf vs independent signed lane model and scalar reference.

Unicorn has no MXU, so only the explicitly delimited extension instructions
are decoded here. Ordinary MIPS, CP0, branches, stack and C dispatch run in
Unicorn. This measures guest instruction work, not H1 cycles or bus timing.
"""
from pathlib import Path
import hashlib,json,random,struct,subprocess,sys
from unicorn import Uc,UC_ARCH_MIPS,UC_MODE_MIPS32,UC_MODE_LITTLE_ENDIAN,UC_HOOK_CODE
from unicorn.mips_const import *
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/minigpt-mxu'
def signed(x,bits):return x-(1<<bits) if x&(1<<(bits-1)) else x
class CPU:
    def __init__(self,build=None,kernel='mg_mxu_dot'):
        build=build or ROOT/'build/minigpt'
        self.uc=Uc(UC_ARCH_MIPS,UC_MODE_MIPS32|UC_MODE_LITTLE_ENDIAN);self.uc.mem_map(0,64*1024*1024)
        self.uc.reg_write(UC_MIPS_REG_CP0_STATUS,0x10000000)
        blob=(build/'MiniGPT.bin').read_bytes();self.write(0x83c00020,blob)
        raw=subprocess.check_output([toolchain_tool('nm'),build/'MiniGPT.elf'],text=True)
        self.symbols={v[2]:int(v[0],16) for l in raw.splitlines() if len(v:=l.split())==3}
        self.xr=[0]+[(i*0x1234abcd)&0xffffffff for i in range(1,16)]+[0]
        self.stats={};self.calls=0;self.vector_macs=0;self.done=False;self.corrupt=False
        self.ranges=[]
        for name,marker,packed in [('mg_mxu_dot',0x4d475832,False),('mg_mxu_q4_dot',0x4d475432,True),('mg_mxu_q4_pair',0x4d475434,True)]:
            if name in self.symbols:
                entry=self.symbols[name];end=0x83c00020+blob.index(struct.pack('<I',marker),entry-0x83c00020)
                self.ranges.append((entry,end,packed))
                if name==kernel:self.entry,self.end=entry,end
        self.uc.hook_add(UC_HOOK_CODE,self.hook)
    def write(self,a,b):self.uc.mem_write(a&0x1fffffff,b)
    def read(self,a,n):return bytes(self.uc.mem_read(a&0x1fffffff,n))
    def word(self,a,v):self.write(a,struct.pack('<I',v&0xffffffff))
    def reg(self,n):return self.uc.reg_read(UC_MIPS_REG_0+n)&0xffffffff
    def hook(self,uc,a,size,_):
        if a==0x80001ff0:self.done=True;uc.emu_stop();return
        self.stats['instructions']=self.stats.get('instructions',0)+1
        region=next((r for r in self.ranges if r[0]<=a<r[1]),None)
        if region is None:return
        entry,end,packed=region
        if a==entry:
            self.saved=(self.xr[:],uc.reg_read(UC_MIPS_REG_CP0_STATUS),self.reg(29),self.reg(4),self.reg(5),self.reg(6),self.reg(7),self.symbols.get('mg_mxu_q4_pair')==a);self.calls+=1
        opword=struct.unpack('<I',self.read(a,4))[0]
        if opword==0x03e00008:
            assert (self.xr,uc.reg_read(UC_MIPS_REG_CP0_STATUS))==self.saved[:2],'MXU/CP0 state changed'
            assert self.reg(29)==self.saved[2]-(80 if self.saved[7] else 64)
        if opword>>26!=28:return
        op=opword&63;a1=(opword>>6)&15;b=(opword>>10)&15;c=(opword>>14)&15;d=(opword>>18)&15
        assert uc.reg_read(UC_MIPS_REG_CP0_STATUS)&1==0
        if op in (0x2e,0x2f):
            n=(opword>>6)&31;g=(opword>>16)&31;assert n<=16
            if op==0x2e:uc.reg_write(UC_MIPS_REG_0+g,self.xr[n])
            elif n:self.xr[n]=self.reg(g)
        else:
            assert self.xr[16]&1
            if op==0x10:
                off=signed((opword>>10)&1023,10)*4;addr=(self.reg((opword>>21)&31)+off)&0xffffffff
                w,q,n=self.saved[3:6]
                q1=n if self.saved[7] else None
                if self.saved[7]:n=64
                assert (w<=addr and addr+4<=w+(n//2 if packed else n)) or (q<=addr and addr+4<=q+2*n) or (q1 is not None and q1<=addr and addr+4<=q1+128),'out-of-range vector load'
                self.xr[a1]=struct.unpack('<I',self.read(addr,4))[0]
            elif op==0x3d:
                pattern=(opword>>24)&3;vb,vc=self.xr[b],self.xr[c]
                if pattern==0:
                    assert b==c
                    vbytes=[(vb>>(8*i))&255 for i in range(4)]
                    self.xr[a1]=vbytes[2]*257|(vbytes[3]*257<<16)
                    self.xr[d]=vbytes[0]*257|(vbytes[1]*257<<16)
                else:
                    assert pattern==3
                    self.xr[a1]=(vb&0xffff0000)|(vc>>16)
                    self.xr[d]=((vb&65535)<<16)|(vc&65535)
            elif op==0x34:
                shift=(opword>>22)&15;vb,vc=self.xr[b],self.xr[c]
                def shl(v):return ((v&65535)<<shift&65535)|(((v>>16)<<shift&65535)<<16)
                self.xr[a1]=shl(vb);self.xr[d]=shl(vc)
            elif op==0x37:
                shift=(opword>>22)&15;vb,vc=self.xr[b],self.xr[c]
                def sar(v):return ((signed(v&65535,16)>>shift)&65535)|(((signed(v>>16,16)>>shift)&65535)<<16)
                self.xr[a1]=sar(vb);self.xr[d]=sar(vc)
            elif op==0x0a:
                assert (opword>>22)&15==0
                vb,vc=self.xr[b],self.xr[c]
                self.xr[a1]=(self.xr[a1]+signed(vb>>16,16)*signed(vc>>16,16))&0xffffffff
                if self.corrupt:self.xr[a1]=(self.xr[a1]+1)&0xffffffff
                self.xr[d]=(self.xr[d]+signed(vb&65535,16)*signed(vc&65535,16))&0xffffffff
                self.vector_macs+=1
            else:raise AssertionError('Unexpected MXU encoding '+hex(opword))
        self.xr[0]=0;uc.reg_write(UC_MIPS_REG_PC,a+4)
    def call(self,name,w,q,n,status=0x10000001):
        self.done=False;self.stats={};sp=0x83aff000
        for reg,value in [(UC_MIPS_REG_A0,w),(UC_MIPS_REG_A1,q),(UC_MIPS_REG_A2,n),(UC_MIPS_REG_SP,sp),(UC_MIPS_REG_RA,0x80001ff0),(UC_MIPS_REG_CP0_STATUS,status)]:self.uc.reg_write(reg,value)
        self.uc.emu_start(self.symbols[name],0,count=300000)
        assert self.done and self.reg(29)==sp
        assert self.uc.reg_read(UC_MIPS_REG_CP0_STATUS)==status
        return signed(self.reg(2),32),self.stats['instructions']
def main():
    OUT.mkdir(exist_ok=True);m=CPU();rng=random.Random(20261006);cases=0;bench=[];w=0x81000000;q=0x81002000
    # Same four-term loop as the pre-MXU engine, under the same GCC flags.
    # The reference includes its ordinary MIPS return/loop overhead; the
    # candidate count includes C dispatch, XR preservation and IRQ handling.
    ref=OUT/'dot-reference.c';ref.write_text('#include <stdint.h>\nint32_t mg_scalar_reference(const int8_t *p,const int16_t *q,int cols){int32_t sum=0;for(int j=0;j<cols;j+=4){sum+=(int32_t)p[j]*q[j];sum+=(int32_t)p[j+1]*q[j+1];sum+=(int32_t)p[j+2]*q[j+2];sum+=(int32_t)p[j+3]*q[j+3];}return sum;}\n',encoding='utf-8')
    elf=OUT/'dot-reference.elf';raw=OUT/'dot-reference.bin';gcc=toolchain_tool('gcc')
    subprocess.run([gcc,'-EL','-march=mips32','-mabi=32','-msoft-float','-mno-abicalls','-G0','-fno-pic','-O2','-ffreestanding','-nostdlib','-I',ROOT/'src/libc/include','-Wl,--build-id=none','-Wl,-Ttext=0x80800000','-Wl,-e,mg_scalar_reference',ref,'-o',elf],check=True)
    subprocess.run([gcc.with_name('mipsel-none-elf-objcopy.exe'),'-O','binary','-j','.text',elf,raw],check=True)
    m.write(0x80800000,raw.read_bytes());m.symbols['mg_scalar_reference']=0x80800000
    for n in [4,8,12,16,32,128,512,1408]:
        for pattern in range(12):
            av=[rng.randrange(-128,128) for _ in range(n)];bv=[rng.randrange(-4095,4096) for _ in range(n)]
            if pattern<4:
                av=[[-128,127,-1,1][pattern]]*n;bv=[4095 if i%2==pattern%2 else -4095 for i in range(n)]
            if pattern==4:av=[-128]*n;bv=[-4095]*n
            if pattern==5:av=[127]*n;bv=[4095]*n
            if pattern==6:av=[0]*n
            a=struct.pack('<'+'b'*n,*av);b=struct.pack('<'+'h'*n,*bv);m.write(w,a);m.write(q,b)
            expected=sum(x*y for x,y in zip(av,bv))
            for irq in (0,1):
                for control in (0,1,6,7):
                    m.xr[16]=control;result,count=m.call('mg_mxu_dot',w,q,n,0x10000400|irq)
                    assert result==expected,(n,pattern,result,expected);cases+=1
            assert m.read(w,n)==a and m.read(q,n*2)==b
            if pattern==11:
                ref,scalar_count=m.call('mg_scalar_reference',w,q,n);assert ref==expected
                m.word(m.symbols['mg_mxu_status'],1)
                ref,mxu_count=m.call('mg_dot',w,q,n);assert ref==expected
                bench.append(dict(terms=n,mxu_leaf_instructions=count,mxu_with_dispatch_instructions=mxu_count,scalar_original_unrolled_instructions=scalar_count,reduction_percent=100*(1-mxu_count/scalar_count)))
    # Execute the actual startup check, then C dispatch, and misaligned fallback.
    m.word(m.symbols['mg_mxu_status'],0);m.call('mg_mxu_init',0,0,0)
    assert struct.unpack('<I',m.read(m.symbols['mg_mxu_status'],4))[0]==1
    assert struct.unpack('<I',m.read(m.symbols['mg_mxu_status']+16,4))[0]==0
    av=[rng.randrange(-128,128) for _ in range(128)];bv=[rng.randrange(-4095,4096) for _ in range(128)]
    a=struct.pack('<128b',*av);b=struct.pack('<128h',*bv);expected=sum(x*y for x,y in zip(av,bv))
    m.write(w,a);m.write(q,b);assert m.call('mg_dot',w,q,128)[0]==expected
    m.write(w+1,a);m.write(q+2,b);assert m.call('mg_dot',w+1,q+2,128)[0]==expected
    # A wrong MXU result must be caught by the real compiled startup check.
    # It then uses scalar dot products without entering the faulty leaf again.
    m.word(m.symbols['mg_mxu_status'],0);m.corrupt=True;m.call('mg_mxu_init',0,0,0)
    assert struct.unpack('<I',m.read(m.symbols['mg_mxu_status'],4))[0]==0xffffffff
    assert struct.unpack('<I',m.read(m.symbols['mg_mxu_status']+16,4))[0]==1
    before=m.calls;m.write(w,a);m.write(q,b)
    assert m.call('mg_dot',w,q,128)[0]==expected and m.calls==before
    report=dict(bda_sha256=hashlib.sha256((ROOT/'dist/MiniGPT.bda').read_bytes()).hexdigest(),environment='Unicorn MIPS32 plus independent signed MXU lane decoder',cases=cases,packed_signed_results_exact=True,irq_status_and_all_xr_control_preserved=True,input_bytes_unchanged=True,no_vector_overread=True,startup_selftest_passed=True,unaligned_scalar_fallback=True,vector_mac_instructions=m.vector_macs,benchmarks=bench,physical_hardware_tested=False,instruction_counts_are_not_cycle_timings=True)
    report['kernel_sha256']=hashlib.sha256(m.read(m.entry,m.end-m.entry)).hexdigest()
    report['injected_bad_mxu_result_rejected_and_scalar_fallback']=True
    (OUT/'kernel-verification.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
