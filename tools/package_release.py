"""Create offline installation packages from this checkout's verified outputs."""
from pathlib import Path
import hashlib,json,re,subprocess,zipfile
ROOT=Path(__file__).resolve().parents[1]
VERSIONS={8:'0.3.6',4:'0.4.2-int4'}
BDAS={8:('MiniGPT.bda','a8d2f782276259b00dff6b58e19dc8044e1d055a6d884bb07392bd1e9d545a67'),4:('MiniGPT-int4.bda','e1e74058b125025abb92ed0b4af27f53712b36564274b4e814e955945c5ec167')}
MODELS={8:'825a306451e14cb2b483bebfd62d4947884f6ac84e4d407f900ed3de9b16052b',4:'b69ee5c66ee2b526e20790a987adcf18357e5c05054403ddfc6aa9a13dedb584'}

def source_files():
    files=[]
    for name in ['README.md','NOTICE.md','LICENSE','requirements.txt','VERSION','.gitmodules','.gitattributes','.gitignore']:
        files.append((ROOT/name,'source/'+name))
    for directory in ['experiments/minigpt','src/runtime','src/libc','src/platform','tools','docs','verification','LICENSES','.github/workflows']:
        for p in sorted((ROOT/directory).rglob('*')):
            if p.is_file() and '__pycache__' not in p.parts:files.append((p,'source/'+p.relative_to(ROOT).as_posix()))
    for directory in ['sdk/include','h1_bda']:
        for p in sorted((ROOT/'sdk'/directory).rglob('*')):
            if p.is_file() and '__pycache__' not in p.parts:files.append((p,'source/sdk/'+p.relative_to(ROOT/'sdk').as_posix()))
    for name in ['LICENSE','NOTICE']:files.append((ROOT/'sdk'/name,'source/sdk/'+name))
    return files

def main():
    out=ROOT/'dist';assets=[];source=source_files()
    for bits in [8,4]:
        name,expected=BDAS[bits];bda=out/name;assert hashlib.sha256(bda.read_bytes()).hexdigest()==expected,(name,'unverified BDA')
        package='MiniGPT' if bits==8 else 'MiniGPT4';model_name='model.mg8' if bits==8 else 'model.mg4';folder=out/package/package
        assert hashlib.sha256((folder/model_name).read_bytes()).hexdigest()==MODELS[bits]
        meta=json.loads(bda.with_suffix('.build.json').read_text(encoding='utf-8'));assert meta['firmware_emulator_verified']
        archive=out/('MiniGPT-H1.zip' if bits==8 else 'MiniGPT-INT4-H1.zip')
        with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
            z.write(bda,'应用/程序/'+name);z.write(bda.with_suffix('.build.json'),name.replace('.bda','.build.json'))
            for f in [model_name,'model.json','prompt.txt']:z.write(folder/f,package+'/'+f)
            z.write(ROOT/'README.md','README.md');z.write(ROOT/'NOTICE.md','NOTICE.md')
            for f in sorted((ROOT/'LICENSES').iterdir()):z.write(f,'LICENSES/'+f.name)
            for f in sorted((ROOT/'verification'/('int8' if bits==8 else 'int4')).rglob('*')):
                if f.is_file():z.write(f,'verification/'+f.name)
            for directory,names in [('minigpt',['host-verification.json','prefill-cache-verification.json']),('minigpt-int4',['quality-verification.json','prefill-cache-verification.json','kernel-verification.json']),('minigpt-layer-cache',['host-verification.json']),('minigpt-mxu',['kernel-verification.json']),('minigpt-mxu-host',['host-verification.json']),('minigpt-int4-host',['host-verification.json'])]:
                for filename in names:
                    f=ROOT/'build'/directory/filename
                    if f.exists():z.write(f,'verification/ci-host/'+directory+'/'+filename)
            for f,n in source:z.write(f,n)
            z.writestr('source/sdk/PINNED_REVISION',meta['sdk_commit']+'\n')
        assets.extend([bda,archive])
    (out/'SHA256SUMS.txt').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.name+'\n' for p in assets),encoding='ascii')
    for p in assets:print(p.name,p.stat().st_size,hashlib.sha256(p.read_bytes()).hexdigest())
    (out/'release-notes.md').write_text("# BBKH1-MiniGPT v"+(ROOT/'VERSION').read_text().strip()+"\n\nINT8 0.3.6 / INT4 0.4.2，适用 H1/Y100 V1.41。\n\n- 原生中文输入法与气泡聊天；设备本地推理。\n- INT8 前缀 KV 复用、连续读取减少定位、启动自动比较额外常驻层与 DMA 双缓冲；可见进度且可跳过。\n- INT4 压缩权重优先全内存、双词元 MXU prefill 与前缀复用。\n- 两份安装 ZIP 含对应模型、许可、验证记录与可重建源码。\n\n从源码构建并与已验证 BDA 的 SHA256 一致；主机测试由本次 Actions 运行。完整固件模拟器证据来自这些确切二进制的本地运行，CI 不运行或分发固件。实机速度需另测。\n\n安装：包内 应用/程序 的 BDA 放到设备同名目录，MiniGPT / MiniGPT4 文件夹放设备盘根目录。两版模型不能混用。\n",encoding='utf-8')
if __name__=='__main__':main()
