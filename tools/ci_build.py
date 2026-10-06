"""Sequential CI build/test/package; fail immediately on any failed command."""
from pathlib import Path
import os,subprocess,sys
ROOT=Path(__file__).resolve().parents[1]

def main():
    if os.environ.get('GITHUB_REF_TYPE')=='tag':
        expected='v'+(ROOT/'VERSION').read_text().strip()
        assert os.environ['GITHUB_REF_NAME']==expected,'Tag must match VERSION'
    commands=[['build_minigpt.py'],['build_minigpt.py','--int4'],['export_release_models.py'],['prepare_test_fixture.py'],['test_minigpt.py'],['test_minigpt_int4.py'],['test_minigpt_prefill_cache.py','--int8'],['test_minigpt_prefill_cache.py'],['test_minigpt_layer_cache.py'],['test_minigpt_dma_auto_host.py','--mxu'],['test_minigpt_dma_auto_host.py','--mxu','--int4'],['test_minigpt_mxu.py'],['test_minigpt_int4_kernel.py'],['package_release.py']]
    for name,*args in commands:
        print('RUN',name,*args,flush=True);subprocess.run([sys.executable,ROOT/'tools'/name,*args],cwd=ROOT,check=True)
if __name__=='__main__':main()
