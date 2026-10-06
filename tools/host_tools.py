from pathlib import Path
import os,shutil
HOST_GCC=os.environ.get('HOST_GCC') or shutil.which('gcc') or shutil.which('gcc.exe')
if not HOST_GCC:raise RuntimeError('Install a native GCC or set HOST_GCC')
def host_env():
    env=os.environ.copy();env['PATH']=str(Path(HOST_GCC).resolve().parent)+os.pathsep+env['PATH'];return env
