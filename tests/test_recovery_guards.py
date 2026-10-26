from pathlib import Path
import tempfile,subprocess,json
HERE=Path(__file__).resolve().parent
root=HERE.parent
s=(root/'app/runtime/quickshare-runner.sh').read_text()
f=s[s.index('recover_controls_only() ('):s.index('if [ "$MODE" = run ]; then')]
setup=r'''
set -eu
RUN=$1; CASE=$2; DRIVER_TOUCHED=0
echo boot1 > "$RUN/original.boot"
die() { echo "ERROR=$1"; exit 10; }
require() { label=$1;shift; "$@" || die "$label"; }
original_hashes() { echo HASH_CHECK;die HASH_CHANGED; }
cat() {
    if [ "$1" = /proc/sys/kernel/random/boot_id ]; then
        if [ "$CASE" = boot_changed ]; then echo boot2; else echo boot1; fi
    else /bin/cat "$@"; fi
}
phase() { echo MUTATION_REACHED;exit 99; }
[ "$CASE" != driver_touched ] || DRIVER_TOUCHED=1
'''
rows={}
for case,error in [('driver_touched','CONTROLS_RECOVERY_DRIVER_TOUCHED'),('boot_changed','CONTROLS_RECOVERY_BOOT_CHANGED'),('hash_changed','HASH_CHANGED')]:
    with tempfile.TemporaryDirectory() as d:
        p=subprocess.run(['bash','-c',setup+f+'\nif recover_controls_only; then echo BAD_SUCCESS; else echo RECOVERY_REFUSED; fi','test',d,case],capture_output=True,text=True,timeout=5)
    assert p.returncode==0 and 'ERROR='+error in p.stdout and 'RECOVERY_REFUSED' in p.stdout and 'MUTATION_REACHED' not in p.stdout and 'BAD_SUCCESS' not in p.stdout,p
    rows[case]=p.stdout
assert s.index('DRIVER_TOUCHED=1',s.index('phase NORMAL_UNLOAD')) < s.index('rmmod qca_cld3_kiwi_v2',s.index('phase NORMAL_UNLOAD'))
assert 'if [ "$DRIVER_TOUCHED" = 0 ] && recover_controls_only; then' in s
(HERE/'RECOVERY-GUARD-TESTS.json').write_text(json.dumps(dict(passed=True,cases=rows),indent=2))
print('RECOVERY_GUARDS_PASSED=3')
