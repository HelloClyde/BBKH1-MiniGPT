"""Build the tested freestanding MiniGPT H1 BDA (packaging is separate)."""
from pathlib import Path
import argparse,hashlib,json,os,struct,subprocess,sys
ROOT=Path(__file__).resolve().parents[1];SRC=ROOT/'experiments/minigpt';SDK=ROOT/'sdk';BUILD=ROOT/'build/minigpt'
sys.path.insert(0,str(ROOT/'.tools/minigpt-deps'))
SDK_REV='067fe072477861dfc8949d7b1a55279fb92d2548'
def run(args):
    p=subprocess.run(list(map(str,args)),capture_output=True,text=True)
    if p.returncode:raise RuntimeError(p.stdout+p.stderr)
    return p
def toolchain_tool(name, directory=None):
    directory=Path(directory or os.environ.get('H1_GNU_BIN',ROOT/'.tools/toolchain/bin')).resolve()
    for suffix in ['.exe','']:
        p=directory/('mipsel-none-elf-'+name+suffix)
        if p.is_file():return p
    raise FileNotFoundError('Missing '+name+' in '+str(directory))

def main():
    global BUILD
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--toolchain',type=Path,default=Path(os.environ.get('H1_GNU_BIN',ROOT/'.tools/toolchain/bin')));parser.add_argument('--no-package',action='store_true')
    parser.add_argument('--profile',action='store_true',help='Separate read-only clock/stage diagnostic BDA')
    parser.add_argument('--full-prefill',action='store_true',help='Diagnostic baseline with the original classifier on every input token')
    parser.add_argument('--dma-pipeline-experiment',action='store_true',help='Separate emulator-only double-buffer/QKV DMA wait-hook prototype')
    parser.add_argument('--dma-auto-experiment',action='store_true',help='Separate experiment that pairs its own guarded RAM hook around each prefetch')
    parser.add_argument('--synchronous',action='store_true',help='Build separate original synchronous diagnostic/reference BDA')
    parser.add_argument('--no-mxu',action='store_true',help='Separate scalar-CPU DMA reference BDA')
    parser.add_argument('--int4',action='store_true',help='Independent packed INT4 application using MiniGPT4/model.mg4')
    args=parser.parse_args()
    if args.full_prefill and not args.profile:parser.error('--full-prefill requires --profile')
    if args.int4 and (args.no_mxu or args.synchronous or args.full_prefill or args.dma_pipeline_experiment or args.dma_auto_experiment):parser.error('INT4 supports the default MXU/DMA build and optional --profile')
    experiment=args.dma_pipeline_experiment or args.dma_auto_experiment
    dma=not args.synchronous
    auto_hook=dma and not args.dma_pipeline_experiment
    mxu=not (args.no_mxu or experiment or args.synchronous)
    if experiment and args.profile:parser.error('DMA experiment is not a timing/profile build')
    if experiment and args.synchronous:parser.error('Synchronous reference cannot use a DMA experiment flag')
    if args.dma_pipeline_experiment and args.dma_auto_experiment:parser.error('Select one DMA variant')
    variant='MiniGPT-dma-auto' if args.dma_auto_experiment else 'MiniGPT-dma-experiment' if args.dma_pipeline_experiment else 'MiniGPT-profile-baseline' if args.full_prefill else 'MiniGPT-profile-scalar' if args.profile and args.no_mxu else 'MiniGPT-profile' if args.profile else 'MiniGPT-sync' if args.synchronous else 'MiniGPT-scalar' if args.no_mxu else 'MiniGPT'
    if args.int4:variant='MiniGPT-int4-profile' if args.profile else 'MiniGPT-int4'
    if args.profile or experiment or args.synchronous or args.no_mxu or args.int4:BUILD=ROOT/'build'/variant.lower()
    BUILD.mkdir(parents=True,exist_ok=True)
    revision=run(['git','-C',SDK,'rev-parse','HEAD']).stdout.strip() if (SDK/'.git').exists() else (SDK/'PINNED_REVISION').read_text().strip()
    if revision!=SDK_REV:raise RuntimeError('SDK revision mismatch')
    gcc=toolchain_tool('gcc',args.toolchain);objcopy=toolchain_tool('objcopy',args.toolchain)
    flags=['-EL','-march=mips32','-mabi=32','-msoft-float','-mno-abicalls','-G0','-fno-pic','-O2','-ffreestanding','-fno-builtin',
           '-fno-stack-protector','-ffunction-sections','-fdata-sections','-fno-strict-aliasing','-g0','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
           f'-ffile-prefix-map={ROOT}=minigpt','-I',SDK/'sdk/include','-I',ROOT/'src/libc/include','-I',ROOT/'src']
    if args.profile:flags.append('-DMG_PROFILE=1')
    if args.full_prefill:flags.append('-DMG_FULL_PREFILL=1')
    if mxu:flags.extend(['-DMG_MXU=1','-DMG_DIAGNOSTICS=1','-I',SRC])
    if args.int4:flags.append('-DMG_INT4=1')
    else:flags.append('-DMG_LAYER_CACHE=1')
    sources=[ROOT/'src/runtime/entry.S',ROOT/'src/runtime/startup.c',ROOT/'src/libc/freestanding.c',SRC/'engine.c',SRC/'perf.c',SRC/'tokenizer.c',SRC/'native_input.c',SRC/'app.c'];objects=[]
    if dma:
        from minigpt_dma_pipeline import write_overlay
        flags.extend(['-DMG_DMA_PIPELINE=1','-I',SRC]);sources[sources.index(SRC/'engine.c')]=write_overlay(BUILD,auto_hook=auto_hook)
        sources.append(SRC/'dma_probe.S')
        if auto_hook:sources.append(SRC/'dma_hook.c')
    if mxu:
        # Distinct object names for the C dispatcher and assembly leaf.
        sources.extend([SRC/'mxu_dot.c',SRC/'mxu_dot.S'])
    if args.int4:sources.extend([SRC/'q4_dot.c',SRC/'q4_mxu.S'])
    for s in sources:
        obj=BUILD/('mg_'+s.stem+('_asm' if s.suffix=='.S' else '')+'.o');run([gcc,*flags,'-c',s,'-o',obj]);objects.append(obj)
    elf=BUILD/'MiniGPT.elf';raw=BUILD/'MiniGPT.bin'
    link_flags=['-Wl,--undefined=dma_probe_trampoline'] if dma else []
    run([gcc,*flags,'-nostdlib','-Wl,--build-id=none','-Wl,--gc-sections',*link_flags,f'-Wl,-T,{ROOT}/src/runtime/h1.ld',f'-Wl,-Map,{BUILD}/MiniGPT.map','-o',elf,*objects,'-lgcc'])
    run([objcopy,'-O','binary',elf,raw]);sys.path.insert(0,str(SDK))
    from h1_bda.header import HeaderFields,encode_header
    from h1_bda.resources import PAYLOAD_OFFSET,RESOURCE_OFFSET,RESOURCE_SIZES,build_icon_resources
    from h1_bda.validate import validate_bda
    payload=raw.read_bytes();size=PAYLOAD_OFFSET+len(payload);padding=(-size)&3
    fields=HeaderFields(category=0x48,file_size_minus_4=size+padding-4,payload_offset=PAYLOAD_OFFSET,resource_offset=RESOURCE_OFFSET,resource_sizes=RESOURCE_SIZES)
    blob=encode_header(fields,title='MiniGPT4' if args.int4 else 'MiniGPT',build_time='2026-10-06 00:00:00')+build_icon_resources(SRC/'icon.png')+payload+bytes(padding)
    dest=ROOT/'dist'/(variant+'.bda');dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(blob);assert validate_bda(dest)['ok']
    digest=hashlib.sha256(blob).hexdigest();dest.with_suffix('.bda.sha256').write_text(digest+'  '+dest.name+'\n',encoding='ascii')
    version='0.4.2-int4' if args.int4 else '0.3.6'
    evidence_path=ROOT/'verification'/('int4' if args.int4 else 'int8')/'native-verification.json'
    evidence=json.loads(evidence_path.read_text(encoding='utf-8'))
    verified=evidence.get('bda_sha256')==digest and evidence.get('response_verified',False)
    meta=dict(version=version,sha256=digest,size=len(blob),sdk_commit=SDK_REV,
              firmware='H1/Y100 V1.41',weight_bits=4 if args.int4 else 8,
              firmware_emulator_verified=verified,physical_hardware_verified=False,
              verification_source='checked-in exact-binary report; CI does not run firmware')
    dest.with_suffix('.build.json').write_text(json.dumps(meta,indent=2)+'\n',encoding='utf-8')
    print(f'PASS BDA {len(blob)} bytes SHA256 {digest}')
if __name__=='__main__':main()
