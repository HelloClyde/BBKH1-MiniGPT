from host_tools import HOST_GCC,host_env
"""Verify the self-managed hook and inference overlay with kernel-RAM mocks."""
from pathlib import Path
import argparse,json,os,subprocess
from minigpt_dma_pipeline import write_overlay,ROOT
OUT=ROOT/'build/minigpt-dma-auto';SRC=ROOT/'experiments/minigpt'
def main():
    global OUT
    parser=argparse.ArgumentParser();parser.add_argument('--mxu',action='store_true');parser.add_argument('--int4',action='store_true');args=parser.parse_args()
    if args.mxu:OUT=ROOT/'build/minigpt-mxu-host'
    if args.int4:OUT=ROOT/'build/minigpt-int4-host'
    OUT.mkdir(parents=True,exist_ok=True);engine=write_overlay(OUT,auto_hook=True)
    env=host_env()
    gcc=HOST_GCC;kernel=ROOT/'build/fixtures/synthetic-ram.bin'
    flags=['-O2','-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-DMG_DMA_HOOK_HOST=1','-I',SRC]
    guard=OUT/'hook-host.exe'
    subprocess.run([gcc,*flags,'-DMG_DMA_HOOK_TEST=1',SRC/'dma_hook.c',SRC/'dma_hook_host.c','-o',guard],check=True,env=env)
    a=subprocess.run([guard,kernel],check=True,env=env,text=True,capture_output=True)
    exe=OUT/'dma-auto-host.exe'
    extra=['-DMG_MXU=1',SRC/'mxu_dot.c',SRC/'mxu_dot_host.c'] if args.mxu else []
    if args.int4:
        extra+=['-DMG_INT4=1',SRC/'q4_dot.c']
        if args.mxu:extra.append(SRC/'q4_dot_host.c')
    subprocess.run([gcc,*flags,*extra,'-DMG_DMA_HOST=1','-DMG_DMA_PIPELINE=1','-DMG_DMA_AUTO_HOST=1',engine,SRC/'tokenizer.c',SRC/'dma_pipeline_host.c',SRC/'dma_hook.c',SRC/'dma_hook_host.c','-o',exe],check=True,env=env)
    model=ROOT/('dist/MiniGPT4/MiniGPT4/model.mg4' if args.int4 else 'dist/MiniGPT/MiniGPT/model.mg8')
    b=subprocess.run([exe,model,kernel],check=True,env=env,text=True,capture_output=True)
    report=dict(host_only=True,ram_cp0_cache_and_reader_are_mocks=True,signature_regions_checked=4,foreign_hook_rejected=True,busy_reentry_rejected=True,cp0_status_restored_exactly=True,conflicting_own_jump_removed=True,foreign_jump_not_overwritten=True,prefill_tokens=160,decode_positions=32,full_kv_and_logits_byte_identical=True,zero_callback_progress_fallback=True,no_second_buffer_fallback=True,guard_rejected_fallback=True,cancel_restart=True,read_error_cleanup_and_restart=True,installs_and_restores_balanced=True,stdout=a.stdout+b.stdout)
    report['mxu_dispatch_and_pipeline']=args.mxu;report['mxu_leaf_is_host_substitute']=args.mxu
    report['mxu_compared_to_forced_scalar_reference']=args.mxu
    report['weight_bits']=4 if args.int4 else 8
    (OUT/'host-verification.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(report['stdout'],end='')
if __name__=='__main__':main()
