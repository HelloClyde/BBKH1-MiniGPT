"""Install the same Windows MIPS GCC archive used for the verified BDA files."""
from pathlib import Path
import argparse,hashlib,urllib.request,zipfile
ROOT=Path(__file__).resolve().parents[1]
URL='https://static.grumpycoder.net/pixel/mips/g++-mipsel-none-elf-15.2.0.zip'
SHA256='8ba866e25c9826ee04ab4310365d264e3e73769e3738bb58ae38fd6740b7ee8d'
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--archive',type=Path);args=parser.parse_args()
    dest=ROOT/'.tools/toolchain';dest.mkdir(parents=True,exist_ok=True)
    archive=args.archive or dest/'g++-mipsel-none-elf-15.2.0.zip'
    if not archive.exists():
        print('Downloading pinned GCC 15.2.0',flush=True)
        request=urllib.request.Request(URL,headers={'User-Agent':'BBKH1-MiniGPT/0.3.6'})
        with urllib.request.urlopen(request,timeout=120) as r,archive.open('wb') as f:
            while chunk:=r.read(1024*1024):f.write(chunk)
    assert hashlib.sha256(archive.read_bytes()).hexdigest()==SHA256,'Toolchain SHA256 mismatch'
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            target=(dest/info.filename).resolve();assert target.is_relative_to(dest.resolve())
        z.extractall(dest)
    gcc=dest/'bin/mipsel-none-elf-gcc.exe';assert gcc.exists()
    print('Toolchain ready:',gcc.parent)
if __name__=='__main__':main()
