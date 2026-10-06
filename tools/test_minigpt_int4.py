from host_tools import HOST_GCC,host_env
"""Check packed Q4 bytes, exact resident/stream inference, NumPy math and Chinese samples."""
from pathlib import Path
import hashlib,json,os,struct,subprocess,sys,time,zlib
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1];sys.path[:0]=[str(ROOT/'.tools/minigpt-deps'),str(ROOT/'tools')]
import numpy as np
import test_minigpt as q8
from export_minigpt import tensors
OUT=ROOT/'build/minigpt-int4';SRC=ROOT/'experiments/minigpt';MODEL=ROOT/'dist/MiniGPT4/MiniGPT4/model.mg4'

def main():
    sys.stdout.reconfigure(encoding='utf-8')
    OUT.mkdir(exist_ok=True);exe=OUT/'quality-host.exe';env=host_env()
    subprocess.run([HOST_GCC,'-O2','-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-DMG_INT4=1','-DMG_HOST_ALLOC_TEST=1','-Wl,--wrap=malloc',SRC/'engine.c',SRC/'q4_dot.c',SRC/'tokenizer.c',SRC/'host.c','-o',exe],check=True,env=env)
    def run(model,prompt,count=32,mode='resident',logits=None):
        argv=[exe,model,prompt,str(count),mode]
        if logits:argv.append(logits)
        return subprocess.run(argv,check=True,capture_output=True,env=env)
    blob=MODEL.read_bytes();assert len(blob)==13819420 and blob[:8]==b'MGPTQ4\0\0'
    assert zlib.crc32(blob[64:])==struct.unpack_from('<I',blob,48)[0]
    original=tensors();decoded={};rows=[]
    names=['model.embed_tokens.weight',None,None]
    for l in range(8):names.extend(f'model.layers.{l}.{n}.weight' for n in ['input_layernorm','self_attn.q_proj','self_attn.k_proj','self_attn.v_proj','self_attn.o_proj','post_attention_layernorm','mlp.gate_proj','mlp.up_proj','mlp.down_proj'])
    for i,name in enumerate(names):
        offset,size,n,c,kind=struct.unpack_from('<5I',blob,64+20*i)
        if not name:continue
        if kind:
            scales=np.frombuffer(blob,dtype='<f2',count=n*c//64,offset=offset).astype(np.float32)
            packed=np.frombuffer(blob,dtype=np.uint8,count=n*c//2,offset=offset+2*n*c//64).reshape(n,c//2)
            low=(packed&15).astype(np.int16);high=(packed>>4).astype(np.int16)
            low[low>=8]-=16;high[high>=8]-=16
            q=np.empty((n,c),np.int16);q[:,::2]=low;q[:,1::2]=high
            a=original[name];expected=np.clip(np.rint(a.reshape(-1,64)/scales[:,None]),-7,7).reshape(n,c)
            assert np.array_equal(q,expected) and np.max(q)<=7 and np.min(q)>=-7
            decoded[name]=(q.astype(np.float32).reshape(-1,64)*scales[:,None]).reshape(n,c)
        else:decoded[name]=np.frombuffer(blob,dtype='<f4',count=n,offset=offset).reshape(original[name].shape)
    decoded['model.norm.weight']=original['model.norm.weight']
    prompts=['你好，请简单介绍一下自己。','什么是人工智能？','一加一等于几？'];records=[]
    for index,text in enumerate(prompts):
        p=OUT/f'quality-prompt-{index}.txt';p.write_text(text,encoding='utf-8');logits=OUT/f'quality-logits-{index}.bin'
        actual=run(MODEL,p,32,'resident',logits);values=np.fromfile(logits,dtype='<f4');assert np.all(np.isfinite(values))
        with patch.object(q8,'tensors',lambda:decoded):reference=q8.reference(text)
        error=float(np.max(np.abs(values-reference)));corr=float(np.corrcoef(values,reference)[0,1]);assert error<0.08 and corr>0.9999,(text,error,corr)
        fp=q8.reference(text);fp_corr=float(np.corrcoef(values,fp)[0,1]);rms=float(np.sqrt(np.mean((values-fp)**2))/np.std(fp))
        stream=run(MODEL,p,32,'stream',OUT/'stream-logits.bin')
        assert np.array_equal(values,np.fromfile(OUT/'stream-logits.bin',dtype='<f4')) and stream.stdout==actual.stdout
        reply=actual.stdout.decode('utf-8',errors='replace');assert any('\u4e00'<=c<='\u9fff' for c in reply)
        record=dict(prompt=text,reply=reply,numpy_q4_max_error=error,numpy_q4_correlation=corr,original_fp32_correlation=fp_corr,original_fp32_normalized_rms_error=rms,first_token_equal_to_fp32=bool(np.argmax(values)==np.argmax(fp)))
        records.append(record);print(json.dumps(record,ensure_ascii=False),flush=True)
    run(MODEL,OUT/'quality-prompt-0.txt',0,'checks')
    bad=OUT/'corrupt.mg4';bad_cases=0
    for off in [0,12,64,2048,len(blob)//2,len(blob)-1]:
        b=bytearray(blob);b[off]^=1;bad.write_bytes(b)
        assert subprocess.run([exe,bad,OUT/'quality-prompt-0.txt','0','stream'],env=env,capture_output=True).returncode==7;bad_cases+=1
    bad.write_bytes(blob[:-1]);assert subprocess.run([exe,bad,OUT/'quality-prompt-0.txt','0','stream'],env=env,capture_output=True).returncode==7;bad.unlink()
    native=run(MODEL,OUT/'quality-prompt-0.txt',96,'resident')
    report=dict(weight_bits=4,model_sha256=hashlib.sha256(blob).hexdigest(),model_bytes=len(blob),quantized_from_original_f16=True,packed_nibbles_match_independent_reference=True,numpy_math_passed=True,resident_stream_logits_and_reply_identical=True,batch_split160_and_allocation_failure_fallback_passed=True,pair_prefill_even_odd_split_and_scratch_allocation_failure_exact=True,prefix_cache_repeated_different_shrinking_singleton_cancel_and_decode_exact=True,cancel_context_bounds_reset=True,corrupt_or_truncated_models_rejected=bad_cases+1,samples=records,native_reply=native.stdout.decode('utf-8',errors='ignore'),native_stderr=native.stderr.decode('utf-8'),native_ai_reply=run(MODEL,OUT/'quality-prompt-1.txt',96,'resident').stdout.decode('utf-8',errors='ignore'),physical_hardware_tested=False)
    (OUT/'quality-verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print('PASS Q4 math, packed format, modes, boundaries and model rejection')
if __name__=='__main__':main()
