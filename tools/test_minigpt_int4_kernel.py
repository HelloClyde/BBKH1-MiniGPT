"""Execute the actual Q4 MIPS unpack/scales and MXU signed kernel independently."""
from pathlib import Path
import hashlib,json,random,struct,sys
from test_minigpt_mxu import CPU,ROOT
sys.path.insert(0,str(ROOT/'.tools/minigpt-deps'))
import numpy as np
from unicorn.mips_const import UC_MIPS_REG_A3
OUT=ROOT/'build/minigpt-int4'
def main():
    m=CPU(OUT,kernel='mg_mxu_q4_dot');m.call('mg_mxu_init',0,0,0);assert struct.unpack('<I',m.read(m.symbols['mg_mxu_status'],4))[0]==1
    rng=random.Random(20261007);packed=0x81000000;q=0x81002000;scales=0x81004000;cases=0;counts={}
    for n in [64,128,512,1408]:
        for pattern in range(12):
            a=[rng.randrange(-8,8) for _ in range(n)];b=[rng.randrange(-4095,4096) for _ in range(n)]
            if pattern<4:a=[[-8,7,-1,0][pattern]]*n;b=[4095 if i%2 else -4095 for i in range(n)]
            raw=bytes((a[i]&15)|((a[i+1]&15)<<4) for i in range(0,n,2));activation=struct.pack('<'+'h'*n,*b)
            m.write(packed,raw);m.write(q,activation);expected=sum(x*y for x,y in zip(a,b))
            for irq in [0,1]:
                for control in [0,1,6,7]:
                    m.xr[16]=control;actual,count=m.call('mg_q4_dot',packed,q,n,0x10000400|irq);assert actual==expected
                    cases+=1
            assert m.read(packed,n//2)==raw and m.read(q,n*2)==activation
            counts[n]=count
            scale_values=np.array([2**-24 if i==0 else (i+1)/1024 for i in range(n//64)],dtype='<f2');m.write(scales,scale_values.tobytes())
            reference=np.float32(0)
            for i,s in enumerate(scale_values.astype(np.float32)):
                value=sum(a[j]*b[j] for j in range(i*64,(i+1)*64));reference=np.float32(reference+np.float32(value)*s)
            m.uc.reg_write(UC_MIPS_REG_A3,n);result,_=m.call('mg_q4_row',packed,scales,q)
            actual=struct.unpack('<f',struct.pack('<I',result&0xffffffff))[0];assert np.float32(actual).tobytes()==reference.tobytes(),(n,actual,reference)
    q1=0x81006000;out=0x81008000;pair_cases=0;pair_instructions=0
    for n in [64,128,512,1408]:
        for pattern in range(12):
            a=[rng.randrange(-8,8) for _ in range(n)]
            b=[rng.randrange(-4095,4096) for _ in range(n)]
            c=[rng.randrange(-4095,4096) for _ in range(n)]
            if pattern<4:
                a=[[-8,7,-1,0][pattern]]*n
                b=[4095 if i&1 else -4095 for i in range(n)]
                c=[-v for v in b]
            raw=bytes((a[i]&15)|((a[i+1]&15)<<4) for i in range(0,n,2))
            m.write(packed,raw);m.write(q,struct.pack('<'+'h'*n,*b));m.write(q1,struct.pack('<'+'h'*n,*c))
            for irq in [0,1]:
                for control in [0,1,6,7]:
                    m.xr[16]=control;m.uc.reg_write(UC_MIPS_REG_A3,out)
                    _,pair_instructions=m.call('mg_mxu_q4_pair',packed,q,q1,0x10000400|irq)
                    assert struct.unpack('<2i',m.read(out,8))==(sum(a[i]*b[i] for i in range(64)),sum(a[i]*c[i] for i in range(64)))
                    pair_cases+=1
            scale_values=np.array([2**-24 if i==0 else (i+1)/1024 for i in range(n//64)],dtype='<f2');m.write(scales,scale_values.tobytes())
            reference=np.zeros(2,dtype='<f4')
            for i,scale in enumerate(scale_values.astype(np.float32)):
                for j,activation in enumerate([b,c]):
                    value=sum(a[t]*activation[t] for t in range(i*64,(i+1)*64))
                    reference[j]=np.float32(reference[j]+np.float32(value)*scale)
            m.word(0x83aff000+16,n);m.word(0x83aff000+20,out);m.uc.reg_write(UC_MIPS_REG_A3,q1)
            m.call('mg_q4_row_pair',packed,scales,q)
            assert m.read(out,8)==reference.tobytes(),(n,pattern,m.read(out,8),reference)
    pair_entry=m.symbols['mg_mxu_q4_pair'];pair_end=next(r[1] for r in m.ranges if r[0]==pair_entry)
    for half in sorted(set([0,1,0x3ff,0x400,0x3c00,0x5640,*range(1,0x5641,29)])):
        result,_=m.call('mg_q4_scale',half,0,0)
        expected=np.frombuffer(struct.pack('<H',half),dtype='<f2').astype('<f4').tobytes();assert struct.pack('<I',result&0xffffffff)==expected,half
    assert m.call('mg_q4_selftest',0,0,0)[0]==1
    report=dict(bda_sha256=hashlib.sha256((ROOT/'dist/MiniGPT-int4.bda').read_bytes()).hexdigest(),kernel_sha256=hashlib.sha256(m.read(m.entry,m.end-m.entry)).hexdigest(),packed_signed_results_exact=True,irq_status_and_all_xr_control_preserved=True,q4_mips_and_independent_mxu_decoder_cases=cases,pair_signed_and_state_cases=pair_cases,pair_group_float_accumulation_byte_identical=True,pair_kernel_sha256=hashlib.sha256(m.read(pair_entry,pair_end-pair_entry)).hexdigest(),pair_leaf_64_instructions=pair_instructions,q4_group_float_accumulation_byte_identical=True,fp16_scale_normal_and_subnormal_conversion_exact=True,startup_selftest_passed=True,guest_instructions_by_dot_length=counts,instruction_counts_are_not_cycle_timings=True,physical_hardware_tested=False)
    (OUT/'kernel-verification.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
