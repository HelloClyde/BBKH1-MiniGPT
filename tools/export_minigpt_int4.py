"""Export packed symmetric per-row INT4 directly from pinned original F16 weights."""
from pathlib import Path
import hashlib,json,struct,zlib
from export_minigpt import ROOT
import export_minigpt as upstream
import numpy as np
OUT=ROOT/'dist/MiniGPT4/MiniGPT4'

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    # Reuse the pinned tensor ordering without touching the existing Q8 model.
    w=upstream.tensors();sequence=[w['model.embed_tokens.weight'],w['model.norm.weight']]
    angle=np.arange(256,dtype=np.float32)[:,None]/(1e6**(np.arange(32,dtype=np.float32)*2/64))
    sequence.append(np.stack([np.cos(angle),np.sin(angle)],axis=1))
    names=['input_layernorm','self_attn.q_proj','self_attn.k_proj','self_attn.v_proj','self_attn.o_proj','post_attention_layernorm','mlp.gate_proj','mlp.up_proj','mlp.down_proj']
    for layer in range(8):sequence.extend(w[f'model.layers.{layer}.{name}.weight'] for name in names)
    entries=[];payload=bytearray();offset=64+75*20;stats=[]
    for i,a in enumerate(sequence):
        matrix=a.ndim==2 and i!=2
        if matrix:
            rows,cols=a.shape;assert cols%8==0
            grouped=a.reshape(-1,64)
            scale=np.maximum(np.max(np.abs(grouped),axis=1)/7,np.float32(2**-24)).astype('<f4')
            # Small models suffer badly with max/7 alone. Pick each row's
            # clipping range by reconstruction MSE using original F16 values.
            base=scale.copy();best=np.full(len(scale),np.inf,np.float32)
            for factor in np.linspace(.5,1.,17,dtype=np.float32):
                candidate=np.maximum((base*factor).astype('<f2').astype(np.float32),np.float32(2**-24))
                values=np.clip(np.rint(grouped/candidate[:,None]),-7,7)
                error=np.mean((values*candidate[:,None]-grouped)**2,axis=1)
                use=error<best;scale[use]=candidate[use];best[use]=error[use]
            q=np.clip(np.rint(grouped/scale[:,None]),-7,7).astype(np.int8).reshape(rows,cols)
            packed=((q[:,0::2].astype(np.uint8)&15)|((q[:,1::2].astype(np.uint8)&15)<<4))
            raw=scale.astype('<f2').tobytes()+packed.tobytes()
            error=(q.reshape(-1,64).astype(np.float32)*scale[:,None]).reshape(a.shape)-a
            stats.append(dict(index=i,normalized_rms_error=float(np.sqrt(np.mean(error**2))/np.sqrt(np.mean(a**2)))))
        else:raw=a.astype('<f4').tobytes();rows=a.size;cols=1
        entries.append((offset+len(payload),len(raw),rows,cols,int(matrix)));payload.extend(raw);payload.extend(bytes((-len(payload))&3))
    size=offset+len(payload);assert size==13819420
    body=b''.join(struct.pack('<5I',*t) for t in entries)+payload
    blob=struct.pack('<8s11If8s',b'MGPTQ4\0\0',2,512,1408,8,8,2,6400,256,75,size,zlib.crc32(body),1e-5,struct.pack('<II',64,16))+body
    (OUT/'model.mg4').write_bytes(blob)
    meta=dict(model=upstream.REPO,revision=upstream.REV,quantization='packed group-64 symmetric INT4 (-7..7), MSE clipping grid 0.5..1.0/17; FP16 group scales; INT16 activations; MXU INT32 dot',context_tokens=256,bytes=size,sha256=hashlib.sha256(blob).hexdigest(),original_sha256=hashlib.sha256((upstream.UPSTREAM/'model.safetensors').read_bytes()).hexdigest(),license='Apache-2.0',tensor_quantization_errors=stats)
    (OUT/'model.json').write_text(json.dumps(meta,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    (OUT/'prompt.txt').write_text('你好，请简单介绍一下自己。',encoding='utf-8')
    print(json.dumps({k:v for k,v in meta.items() if k!='tensor_quantization_errors'},indent=2))
if __name__=='__main__':main()
