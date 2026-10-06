from host_tools import HOST_GCC,host_env
"""Check real INT8 cache allocation, IO savings and inference under bounded RAM."""
from pathlib import Path
import hashlib,json,os,subprocess
from minigpt_dma_pipeline import ROOT,write_overlay

def main():
    out=ROOT/'build/minigpt-layer-cache';out.mkdir(parents=True,exist_ok=True)
    src=ROOT/'experiments/minigpt';engine=write_overlay(out,auto_hook=True)
    env=host_env()
    gcc=HOST_GCC
    flags=['-O2','-fno-strict-aliasing','-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
           '-DMG_LAYER_CACHE=1','-DMG_DMA_PIPELINE=1','-DMG_DMA_HOST=1',
           '-DMG_DMA_HOOK_HOST=1','-DMG_MXU=1','-I',src]
    obj=out/'engine.o';exe=out/'layer-cache-host.exe'
    subprocess.run([gcc,*flags,'-Dmalloc=cache_test_malloc','-Dfree=cache_test_free','-c',engine,'-o',obj],env=env,check=True)
    subprocess.run([gcc,*flags,obj,src/'layer_cache_host.c',src/'dma_hook.c',src/'dma_hook_host.c',src/'mxu_dot.c',src/'mxu_dot_host.c','-o',exe],env=env,check=True)
    run=subprocess.run([exe,ROOT/'dist/MiniGPT/MiniGPT/model.mg8',ROOT/'build/fixtures/synthetic-ram.bin'],env=env,capture_output=True,text=True)
    if run.returncode:raise RuntimeError(run.stdout+run.stderr)
    report=dict(engine_sha256=hashlib.sha256((src/'engine.c').read_bytes()).hexdigest(),
                overlay_sha256=hashlib.sha256(engine.read_bytes()).hexdigest(),host_only=True,
                cached_layer_counts=list(range(9)),full_kv_logits_byte_identical=True,
                per_step_io_bytes_exact=True,cancel_restart=True,allocation_failure_fallback=True,
                read_and_validation_failure_cleanup=True,
                adaptive_cache_and_dma_selection_exact=True,adaptive_cancel_and_read_validation_failure=True,no_memory_leaks=True,
                incomplete_cache_warmup_preserves_model_and_completed_layers=True,
                dma_and_zero_progress_and_no_second_buffer=True,mxu_leaf_is_host_substitute=True,
                reserve_bytes=2*1024*1024,layer_bytes=2840576,stdout=run.stdout)
    (out/'host-verification.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(run.stdout,end='')
if __name__=='__main__':main()
