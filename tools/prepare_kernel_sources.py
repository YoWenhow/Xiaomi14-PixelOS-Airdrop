"""Apply the published overlay to disposable, pinned upstream source checkouts.

No download or device access. Refuses unexpected source bytes before writing.
Run on Linux/WSL (the WLAN build uses two relative symlinks).
"""
from pathlib import Path
import argparse,json,hashlib,shutil
ROOT=Path(__file__).resolve().parents[1]
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
def inside(root,rel):
    path=root/rel
    if not path.resolve().is_relative_to(root.resolve()):raise ValueError('Path escapes source root: '+rel)
    return path
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--wlan',type=Path,required=True);p.add_argument('--kernel',type=Path,required=True)
    a=p.parse_args();w=json.loads((ROOT/'kernel/wlan-source.json').read_text());k=json.loads((ROOT/'kernel/kernel-source.json').read_text())
    # Every unchanged WLAN input must already match the final pinned baseline.
    for rel,digest in w['final_files'].items():
        if rel in w['changed']:continue
        f=inside(a.wlan,rel)
        if not f.is_file() or sha(f)!=digest:raise ValueError('Wrong WLAN baseline: '+rel)
    for rel,digest in k['baseline_changed'].items():
        f=inside(a.kernel,rel)
        if digest is None and not f.exists():continue
        if not f.is_file() or sha(f) not in [digest,k['changed'][rel]]:raise ValueError('Wrong kernel baseline: '+rel)
    for source,dest in [(ROOT/'kernel/wlan-overlay',a.wlan),(ROOT/'kernel/kernel-overlay',a.kernel)]:
        for f in source.rglob('*'):
            if f.is_file():
                out=inside(dest,f.relative_to(source));out.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(f,out)
    for rel,target in w['symlinks'].items():
        f=a.wlan/rel
        if f.is_symlink():
            if str(f.readlink())!=target:raise ValueError('Unexpected symlink: '+rel)
        elif f.exists():raise ValueError('Expected a symlink or absent path: '+rel)
        else:f.symlink_to(target,target_is_directory=True)
    for rel,digest in w['final_files'].items():
        if sha(a.wlan/rel)!=digest:raise ValueError('Final WLAN mismatch: '+rel)
    print('WLAN_SOURCE_VERIFIED='+str(len(w['final_files'])))
if __name__=='__main__':main()
