from host_tools import HOST_GCC,host_env
"""Compare actual MiniMind2-Small C inference with an independent FP32 NumPy Llama.

Also check GPT-2 ByteLevel BPE against the pinned HuggingFace tokenizer, including
UTF-8, whitespace, specials, mixed languages and bounded malformed input.
"""
from pathlib import Path
import json, os, random, struct, subprocess, sys, time
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'.tools/minigpt-deps'),str(ROOT/'tools')]
import numpy as np
from tokenizers import Tokenizer
from export_minigpt import tensors
BUILD=ROOT/'build/minigpt';SRC=ROOT/'experiments/minigpt'
EXE=BUILD/'host.exe'

def compile_host():
    subprocess.run([HOST_GCC,'-O2','-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-DMG_HOST_ALLOC_TEST=1','-Wl,--wrap=malloc',SRC/'engine.c',SRC/'tokenizer.c',SRC/'host.c','-o',EXE],check=True,env=host_env())

def run(args):
    env=host_env()
    return subprocess.run([EXE,*map(str,args)],env=env,capture_output=True,check=True)

def token_tests():
    tok=Tokenizer.from_file(str(BUILD/'upstream/tokenizer.json'));rng=random.Random(2026)
    cases=['你好，请简单介绍一下自己。',"I'm don't we're I'M",'中文 English123，测试！🙂',' \t  x\n\n \t',
           '<|im_start|>system\nYou are a helpful assistant<|im_end|>\n', ' <|im_start|>user\n你好<|im_end|>',
           'abc<|im_end|>def','一二三\u3000四五六','Áé\u0301\u202f१२३\n\r\t中文', '', '  hello   world  ', '\t\tABC']
    for _ in range(100):cases.append(''.join(rng.choice('你好吗世界helloABC123 .,!?\n\t\u3000🙂') for _ in range(rng.randrange(1,60))))
    tmp=BUILD/'token-test.txt'
    for text in cases:
        tmp.write_bytes(text.encode('utf-8'));result=list(map(int,run(['--encode',tmp]).stdout.split()))
        expected=tok.encode(text,add_special_tokens=False).ids
        assert result==[len(expected),*expected],(repr(text),result,expected)
    tmp.write_bytes(b'\xc0\xaf');assert subprocess.run([EXE,'--encode',tmp],capture_output=True).returncode!=0
    return len(cases)

def reference(prompt,quantized=False):
    w=tensors();tok=Tokenizer.from_file(str(BUILD/'upstream/tokenizer.json'))
    if quantized:
        for name,a in w.items():
            if a.ndim==2:
                scale=np.maximum(np.max(np.abs(a),axis=1)/127,np.float32(1e-20))
                w[name]=np.rint(a/scale[:,None]).clip(-127,127)*scale[:,None]
    ids=tok.encode('<|im_start|>system\nYou are a helpful assistant<|im_end|>\n<|im_start|>user\n'+prompt+'<|im_end|>\n<|im_start|>assistant\n',add_special_tokens=False).ids
    x=w['model.embed_tokens.weight'][ids].copy();n=len(ids)
    cs=np.cos(np.arange(n,dtype=np.float32)[:,None]/(1e6**(np.arange(32,dtype=np.float32)*2/64)))[:,None,:]
    sn=np.sin(np.arange(n,dtype=np.float32)[:,None]/(1e6**(np.arange(32,dtype=np.float32)*2/64)))[:,None,:]
    def norm(a,weight):return a/(np.sqrt(np.mean(a*a,axis=-1,keepdims=True)+np.float32(1e-5)))*weight
    def rotate(a):
        a,b=np.split(a,2,axis=-1);return np.concatenate([a*cs-b*sn,b*cs+a*sn],axis=-1)
    for l in range(8):
        prefix=f'model.layers.{l}.';z=norm(x,w[prefix+'input_layernorm.weight'])
        q=rotate((z@w[prefix+'self_attn.q_proj.weight'].T).reshape(n,8,64))
        k=rotate((z@w[prefix+'self_attn.k_proj.weight'].T).reshape(n,2,64)).repeat(4,axis=1)
        v=(z@w[prefix+'self_attn.v_proj.weight'].T).reshape(n,2,64).repeat(4,axis=1)
        scores=np.einsum('thd,shd->hts',q,k)*np.float32(0.125)
        scores=np.where(np.tri(n,dtype=bool)[None],scores,-1e30);scores-=scores.max(axis=-1,keepdims=True)
        scores=np.exp(scores);scores/=scores.sum(axis=-1,keepdims=True)
        a=np.einsum('hts,shd->thd',scores,v).reshape(n,512)
        x+=a@w[prefix+'self_attn.o_proj.weight'].T
        z=norm(x,w[prefix+'post_attention_layernorm.weight']);gate=z@w[prefix+'mlp.gate_proj.weight'].T
        up=z@w[prefix+'mlp.up_proj.weight'].T
        x+=(gate/(1+np.exp(-gate))*up)@w[prefix+'mlp.down_proj.weight'].T
    return norm(x[-1],w['model.norm.weight'])@w['model.embed_tokens.weight'].T

def main():
    BUILD.mkdir(parents=True,exist_ok=True);sys.stdout.reconfigure(encoding='utf-8');compile_host();count=token_tests();print('Tokenizer PASS',count,flush=True)
    prompt='你好，请简单介绍一下自己。';p=BUILD/'test-prompt.txt';p.write_text(prompt,encoding='utf-8')
    model=ROOT/'dist/MiniGPT/MiniGPT/model.mg8';log=BUILD/'logits.bin';start=time.monotonic()
    actual=run([model,p,48,'resident',log]);q=np.fromfile(log,dtype='<f4');fp=reference(prompt)
    error=float(np.max(np.abs(q-fp)));corr=float(np.corrcoef(q,fp)[0,1]);print('FP32 reference vs Q8:',error,corr)
    normalized_rms=float(np.sqrt(np.mean((q-fp)**2))/np.std(fp))
    assert corr>0.999 and normalized_rms<0.05,(error,corr,normalized_rms)
    assert int(np.argmax(q))==int(np.argmax(fp))
    quant_fp=reference(prompt,True);quant_error=float(np.max(np.abs(q-quant_fp)))
    assert quant_error<0.05,quant_error
    stream=run([model,p,8,'stream',BUILD/'stream-logits.bin']);assert np.array_equal(q,np.fromfile(BUILD/'stream-logits.bin',dtype='<f4'))
    baseline=run([model,p,64,'stream',BUILD/'full-prefill-logits.bin','full-prefill'])
    assert np.array_equal(q,np.fromfile(BUILD/'full-prefill-logits.bin',dtype='<f4'))
    assert baseline.stdout==actual.stdout, 'Optimized prefill changed generated tokens'
    run([model,p,0,'checks'])
    blob=model.read_bytes();bad=BUILD/'corrupt.mg8';bad_cases=0
    for offset in [0,12,64,2048,len(blob)//2,len(blob)-1]:
        copy=bytearray(blob);copy[offset]^=1;bad.write_bytes(copy)
        assert subprocess.run([EXE,bad,p,'0','stream'],capture_output=True).returncode==7,offset
        bad_cases+=1
    bad.write_bytes(blob[:-1]);assert subprocess.run([EXE,bad,p,'0','stream'],capture_output=True).returncode==7
    bad.unlink()
    reply=actual.stdout.decode('utf-8',errors='strict');print(reply);print(actual.stderr.decode())
    assert any('\u4e00'<=c<='\u9fff' for c in reply)
    report=dict(tokenizer_cases=count,reference='FP32 NumPy Llama on original F16 weights',logit_max_error=error,logit_correlation=corr,
                normalized_logit_rms_error=normalized_rms,q8_numpy_max_error=quant_error,
                first_token_equal=True,stream_resident_logits_identical=True,prefill_baseline_logits_and_reply_identical=True,batch_prefill_split160_logits_and_kv_identical=True,batch_prefill_cancel_and_bounds=True,batch_allocation_failure_serial_fallback_identical=True,cancel_context_bounds_reset=True,corrupt_or_truncated_models_rejected=bad_cases+1,
                prompt=prompt,reply=reply,host_seconds=time.monotonic()-start)
    (BUILD/'host-verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print('PASS')
if __name__=='__main__':main()
