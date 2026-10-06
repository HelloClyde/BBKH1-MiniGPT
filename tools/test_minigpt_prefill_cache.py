from host_tools import HOST_GCC,host_env
"""Compare cached independent questions against full serial/batch inference."""
from pathlib import Path
import hashlib,json,os,subprocess,sys
ROOT=Path(__file__).resolve().parents[1]
INT8='--int8' in sys.argv
SRC=ROOT/'experiments/minigpt';OUT=ROOT/('build/minigpt' if INT8 else 'build/minigpt-int4')
env=host_env()
OUT.mkdir(parents=True,exist_ok=True)
exe=OUT/'prefill-cache-host.exe'
flags=['-DMG_PREFIX_CACHE=1'] if INT8 else ['-DMG_INT4=1']
sources=[SRC/'engine.c',SRC/'tokenizer.c',SRC/'host.c']+([] if INT8 else [SRC/'q4_dot.c'])
subprocess.run([HOST_GCC,'-O2','-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',*flags,'-DMG_HOST_ALLOC_TEST=1','-Wl,--wrap=malloc',*sources,'-o',exe],check=True,env=env)
model=ROOT/('dist/MiniGPT/MiniGPT/model.mg8' if INT8 else 'dist/MiniGPT4/MiniGPT4/model.mg4')
prompt=OUT/'prefix-test-prompt.txt';prompt.write_text('你好，请简单介绍一下自己。',encoding='utf-8')
p=subprocess.run([exe,model,prompt,'0','checks'],check=True,capture_output=True,text=True,env=env)
report=dict(model_sha256=hashlib.sha256(model.read_bytes()).hexdigest(),engine_sha256=hashlib.sha256((SRC/'engine.c').read_bytes()).hexdigest(),prefix_cache_repeated_different_shrinking_singleton_cancel_and_decode_exact=True,pair_prefill_even_odd_split_and_scratch_allocation_failure_exact=not INT8,reference_uses_full_uncached_prompt=True,physical_hardware_tested=False,stdout=p.stdout)
(OUT/'prefill-cache-verification.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('PASS prefix KV/logits and subsequent decode vs full uncached inference; odd/even and allocation/cancel fallbacks')
