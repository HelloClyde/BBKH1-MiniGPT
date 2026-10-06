"""Pin/download MiniMind2-Small and export the H1 Q8 weight format and tokenizer.

Dependencies: numpy, tokenizers (only for validation), Pillow is not required.
No PyTorch or executable remote model code is used.
"""
from pathlib import Path
import gzip, hashlib, io, json, struct, sys, tarfile, unicodedata, urllib.request, zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / '.tools/minigpt-deps'))
import numpy as np

REV = '8c0c0de640cd532fee03d329b95a58c56591b5bc'
REPO = 'jingyaogong/MiniMind2-Small'
SOURCE = ROOT / 'experiments/minigpt'
UPSTREAM = ROOT / 'build/minigpt/upstream'
OUT = ROOT / 'dist/MiniGPT/MiniGPT'
CONTEXT = 256

def download(name):
    dest = UPSTREAM / name
    if not dest.exists():
        with urllib.request.urlopen(f'https://huggingface.co/{REPO}/resolve/{REV}/{name}', timeout=120) as r:
            dest.write_bytes(r.read())
    return dest

def tensors():
    data = download('model.safetensors').read_bytes()
    assert hashlib.sha256(data).hexdigest()=='83bfe6f127c98120a3410aab65eee3b66b8301ac352c20b7efd5e2eb688f6d85', 'Pinned weights hash mismatch'
    size, = struct.unpack_from('<Q', data)
    header = json.loads(data[8:8+size])
    result = {}
    for name, desc in header.items():
        if name == '__metadata__': continue
        assert desc['dtype'] == 'F16', desc
        a,b = desc['data_offsets']
        result[name] = np.frombuffer(data, dtype='<f2', count=(b-a)//2, offset=8+size+a).astype(np.float32).reshape(desc['shape'])
    return result

def weights():
    config = json.loads(download('config.json').read_text(encoding='utf-8'))
    expected = dict(hidden_size=512, intermediate_size=1408, num_hidden_layers=8,
                    num_attention_heads=8, num_key_value_heads=2, vocab_size=6400,
                    rope_theta=1000000.0, tie_word_embeddings=True)
    assert all(config[k] == v for k,v in expected.items()), config
    w = tensors()
    sequence = [w['model.embed_tokens.weight'], w['model.norm.weight']]
    angle = np.arange(CONTEXT, dtype=np.float32)[:,None] / (1e6 ** (np.arange(32, dtype=np.float32)*2/64))
    sequence.append(np.stack([np.cos(angle), np.sin(angle)], axis=1))
    names = ['input_layernorm', 'self_attn.q_proj', 'self_attn.k_proj', 'self_attn.v_proj',
             'self_attn.o_proj', 'post_attention_layernorm', 'mlp.gate_proj', 'mlp.up_proj', 'mlp.down_proj']
    for layer in range(8):
        sequence.extend(w[f'model.layers.{layer}.{name}.weight'] for name in names)
    assert len(sequence) == 75
    payload = bytearray(); entries = []
    offset = 64 + len(sequence)*20
    for i,a in enumerate(sequence):
        matrix = a.ndim == 2 and i != 2
        if matrix:
            scale = np.maximum(np.max(np.abs(a),axis=1)/127, np.float32(1e-20)).astype('<f4')
            q = np.clip(np.rint(a/scale[:,None]), -127,127).astype(np.int8)
            raw = scale.tobytes()+q.tobytes()
            rows,cols = a.shape
        else:
            raw = a.astype('<f4').tobytes();rows=a.size;cols=1
        entries.append((offset+len(payload),len(raw),rows,cols,int(matrix)))
        payload.extend(raw)
        payload.extend(bytes((-len(payload))&3))
    size = offset+len(payload)
    body=b''.join(struct.pack('<5I',*entry) for entry in entries)+payload
    header = struct.pack('<8s11If8s', b'MGPTQ8\0\0', 1,512,1408,8,8,2,6400,CONTEXT,75,size,zlib.crc32(body),1e-5,bytes(8))
    assert len(header)==64
    blob = header+body
    dest=OUT/'model.mg8';dest.write_bytes(blob)
    return dict(model=REPO, revision=REV, quantization='per-row symmetric INT8 weights; dynamic INT16 activations (4095); INT32 dot; FP32 RMSNorm/attention/SwiGLU',
                context_tokens=CONTEXT, bytes=len(blob),sha256=hashlib.sha256(blob).hexdigest(),
                original_sha256=hashlib.sha256(download('model.safetensors').read_bytes()).hexdigest(), license='Apache-2.0')

def array(name,ctype,values):
    return f'static const {ctype} {name}[] = {{\n'+',\n'.join(','.join(str(v) for v in values[i:i+24]) for i in range(0,len(values),24))+'\n};\n'

def tokenizer():
    t=json.loads(download('tokenizer.json').read_text(encoding='utf-8'));v=t['model']['vocab']
    assert t['pre_tokenizer']==dict(type='ByteLevel',add_prefix_space=False,trim_offsets=True,use_regex=True)
    bs=list(range(33,127))+list(range(161,173))+list(range(174,256));cs=bs[:];n=0
    for b in range(256):
        if b not in bs:bs.append(b);cs.append(256+n);n+=1
    inverse=dict(zip(map(chr,cs),bs));byteids=[v[chr(cs[bs.index(i)])] for i in range(256)]
    raw=bytearray();offsets=[0]
    for token,index in sorted(v.items(), key=lambda item:item[1]):
        assert index==len(offsets)-1
        raw.extend(token.encode() if index<3 else bytes(inverse[c] for c in token));offsets.append(len(raw))
    keys=[0]*16384;values=[0]*16384;ranks=[0]*16384
    for rank,(a,b) in enumerate(t['model']['merges']):
        key=1+v[a]+(v[b]<<13);slot=((key*2654435761)&0xffffffff)>>18
        while keys[slot]:slot=(slot+1)&16383
        keys[slot]=key;values[slot]=v[a+b];ranks[slot]=rank
    text='/* Generated from pinned MiniMind2-Small tokenizer.json. */\n'
    for name,ctype,a in [('token_offsets','uint32_t',offsets),('token_bytes','uint8_t',list(raw)),('byte_tokens','uint16_t',byteids),('merge_keys','uint32_t',keys),('merge_values','uint16_t',values),('merge_ranks','uint16_t',ranks)]:text+=array(name,ctype,a)
    # GPT-2 ByteLevel regex uses Unicode L, N, White_Space. Generate range table.
    ranges=[];start=0;last=0
    def kind(cp):
        c=chr(cp);cat=unicodedata.category(c)
        return 1 if cat.startswith('L') else 2 if cat.startswith('N') else 3 if c in '\t\n\v\f\r ' or cp in [0x85,0xa0,0x1680,0x2028,0x2029,0x202f,0x205f,0x3000] or 0x2000<=cp<=0x200a else 0
    for cp in range(0x110001):
        k=kind(cp) if cp<0x110000 else -1
        if k!=last:
            if last:ranges.append((start,cp-1,last))
            start=cp;last=k
    text+='static const struct {uint32_t first,last;uint8_t kind;} categories[]={\n'+''.join('{%d,%d,%d},\n'%r for r in ranges)+'};\n'
    (SOURCE/'token_data.h').write_text(text,encoding='ascii')

def font():
    path=UPSTREAM/'unifont.hex.gz'
    if not path.exists():
        path.write_bytes(urllib.request.urlopen('https://unifoundry.com/pub/unifont/unifont-16.0.04/font-builds/unifont-16.0.04.hex.gz',timeout=60).read())
    rows=[]
    for line in gzip.decompress(path.read_bytes()).decode('ascii').splitlines():
        cp,hexdata=line.split(':');cp=int(cp,16)
        if not (32<=cp<=126 or 0x2000<=cp<=0x9fff or 0xff00<=cp<=0xffef):continue
        a=bytes.fromhex(hexdata)
        if len(a)==16:bits=list(a);width=8
        else:bits=[a[i]*256+a[i+1] for i in range(0,32,2)];width=16
        rows.append('{%d,%d,{%s}},\n'%(cp,width,','.join(map(str,bits))))
    (SOURCE/'font_data.h').write_text('/* MiniGPT UI Bitmap: GNU Unifont 16.0.04 subset, SIL Open Font License 1.1. */\nstatic const struct {uint16_t cp;uint8_t width;uint16_t rows[16];} glyphs[]={\n'+''.join(rows)+'};\n',encoding='ascii')

def gbk_table():
    # Native H1 GUI strings are CP936; the model tokenizer consumes UTF-8.
    pairs=[]
    for lead in range(0x81,0xff):
        for trail in range(0x40,0xff):
            if trail==0x7f:continue
            try:s=bytes([lead,trail]).decode('gbk')
            except UnicodeDecodeError:continue
            if len(s)==1:pairs.append((lead*256+trail,ord(s)))
    (SOURCE/'gbk_data.h').write_text('/* CP936 mapping generated with Python gbk codec; integer character mappings. */\nstatic const unsigned short gbk_pairs[][2]={\n'+''.join('{0x%04x,0x%04x},\n'%x for x in pairs)+'};\n',encoding='ascii')

def licenses():
    dest=UPSTREAM/'LICENSE'
    if not dest.exists():
        url='https://raw.githubusercontent.com/jingyaogong/minimind/f659b55761b754d306bd140573493a6543cafd7f/LICENSE'
        dest.write_bytes(urllib.request.urlopen(url,timeout=60).read())
    if not (UPSTREAM/'unifont-COPYING').exists() or not (UPSTREAM/'unifont-README').exists():
        tar=UPSTREAM/'unifont.tar.gz'
        if not tar.exists():tar.write_bytes(urllib.request.urlopen('https://unifoundry.com/pub/unifont/unifont-16.0.04/unifont-16.0.04.tar.gz',timeout=120).read())
        raw=tar.read_bytes()
        with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
            for name,dest in [('COPYING','unifont-COPYING'),('README','unifont-README')]:
                (UPSTREAM/dest).write_bytes(archive.extractfile('unifont-16.0.04/'+name).read())

def main():
    UPSTREAM.mkdir(parents=True,exist_ok=True);OUT.mkdir(parents=True,exist_ok=True)
    meta=weights();tokenizer();font();gbk_table();licenses()
    (OUT/'model.json').write_text(json.dumps(meta,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    (OUT/'prompt.txt').write_text('你好，请简单介绍一下自己。',encoding='utf-8')
    print(json.dumps(meta,indent=2))

if __name__=='__main__':main()
