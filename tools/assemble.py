"""Offline local assembly. No downloads, adb, fastboot, root, or device access.

Requires user-provided proprietary inputs; never changes compatibility guards.
"""
from pathlib import Path
import argparse, hashlib, json, os, secrets, shlex, shutil, subprocess, sys, zipfile
import profile_checks as checks

ROOT = Path(__file__).resolve().parents[1]
def put(p, s):
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(s, encoding='utf-8', newline='\n')
def digest(p):
    with p.open('rb') as f:return hashlib.file_digest(f, 'sha256').hexdigest()
def run(argv, env=None):
    r=subprocess.run([str(a) for a in argv],env=env,capture_output=True,text=True,encoding='utf-8',errors='replace')
    if r.returncode:raise RuntimeError(f'{Path(str(argv[0])).name} failed:\n{r.stdout}\n{r.stderr}')
    return r.stdout
def tool(path):return path.with_suffix('.exe') if os.name=='nt' else path
def manifest(root, target, exclude=()):
    checks.manifest(root,target,set(exclude))
def zip_tree(root, dest):
    with zipfile.ZipFile(dest,'x',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for p in sorted(root.rglob('*')):
            if p.is_file():
                info=zipfile.ZipInfo(p.relative_to(root).as_posix(),(2026,10,26,0,0,0))
                info.create_system=3;info.external_attr=(0o100755 if p.suffix=='.sh' else 0o100644)<<16
                info.compress_type=zipfile.ZIP_DEFLATED;z.writestr(info,p.read_bytes())
def resolve_inputs(open_runtime, dependencies):
    spec=json.loads((ROOT/'profiles/runtime-inputs.json').read_text())
    found={};errors=[]
    for rel,item in spec.items():
        p=(open_runtime/rel) if item['redistributable'] else (dependencies/Path(rel).name)
        if not p.is_file() or p.is_symlink():errors.append('missing ordinary input: '+str(p));continue
        if p.stat().st_size!=item['bytes'] or digest(p)!=item['sha256']:errors.append('hash/size mismatch: '+rel);continue
        found[rel]=p
    if errors:raise ValueError('\n'.join(errors))
    return found
def build_app(inputs, out, sdk, jdk):
    bt=sdk/'build-tools/36.0.0';android=sdk/'platforms/android-36/android.jar'
    for p in [tool(jdk/'javac'),tool(jdk/'java'),tool(jdk/'keytool'),tool(bt/'aapt2'),tool(bt/'zipalign'),bt/'lib/d8.jar',bt/'lib/apksigner.jar',android]:
        if not p.is_file():raise ValueError('Missing Android/JDK tool: '+str(p))
    classes=out/'classes';dex=out/'dex';assets=out/'assets'
    for p in [classes,dex,assets]:p.mkdir(parents=True)
    run([tool(jdk/'javac'),'--release','8','-encoding','UTF-8','-cp',android,'-d',classes,*sorted((ROOT/'app/src').rglob('*.java'))])
    run([tool(jdk/'java'),'-cp',bt/'lib/d8.jar','com.android.tools.r8.D8','--min-api','36','--lib',android,'--output',dex,*sorted(classes.rglob('*.class'))])
    for rel,p in inputs.items():
        if rel.startswith(('runtime/','quickshare-runtime/')):
            dest=assets/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,dest)
    for sub,script in [('runtime','phone-runner.sh'),('quickshare-runtime','quickshare-runner.sh')]:
        shutil.copyfile(ROOT/'app/runtime'/script,assets/sub/script)
    shutil.copyfile(dex/'classes.dex',assets/'runtime/mobile-classes.dex')
    for sub in ['runtime','quickshare-runtime']:
        p=assets/sub
        put(p/'SHA256SUMS',''.join(digest(f)+'  '+f.relative_to(p).as_posix()+'\n' for f in sorted(p.rglob('*')) if f.is_file()))
    run([tool(bt/'aapt2'),'compile','--dir',ROOT/'app/res','-o',out/'resources.zip'])
    run([tool(bt/'aapt2'),'link','-o',out/'unsigned.apk','-I',android,'--manifest',ROOT/'app/AndroidManifest.xml','-A',assets,out/'resources.zip'])
    with zipfile.ZipFile(out/'unsigned.apk','a',zipfile.ZIP_DEFLATED) as z:z.write(dex/'classes.dex','classes.dex')
    run([tool(bt/'zipalign'),'-f','4',out/'unsigned.apk',out/'aligned.apk'])
    private=ROOT/'private';private.mkdir(exist_ok=True)
    password_file=private/'signing-password.txt';keystore=private/'local-release.p12'
    if keystore.exists() and not password_file.exists():raise ValueError('Signing password missing; keep the existing keystore safe')
    if not password_file.exists():
        password_file.write_text(secrets.token_hex(24),encoding='ascii');password_file.chmod(0o600)
    env=dict(os.environ,HOUJI_SIGN_PASS=password_file.read_text().strip())
    if not keystore.exists():
        run([tool(jdk/'keytool'),'-genkeypair','-keystore',keystore,'-storetype','PKCS12','-storepass:env','HOUJI_SIGN_PASS','-keypass:env','HOUJI_SIGN_PASS','-alias','local-release','-keyalg','RSA','-keysize','3072','-validity','3650','-dname','CN=Houji Local Build'],env)
        keystore.chmod(0o600)
    apk=out/'companion.apk'
    run([tool(jdk/'java'),'-jar',bt/'lib/apksigner.jar','sign','--ks',keystore,'--ks-key-alias','local-release','--ks-pass','env:HOUJI_SIGN_PASS','--out',apk,out/'aligned.apk'],env)
    run([tool(jdk/'java'),'-jar',bt/'lib/apksigner.jar','verify','--verbose',apk])
    run([tool(bt/'zipalign'),'-c','4',apk])
    return apk
def build_module(profile, system_ext, app, inputs, out):
    profile=dict(profile,system_ext_root=str(system_ext),app_apk=str(app),app_sha256=digest(app))
    checks.validate_profile(profile)
    mod=out/'module';shutil.copytree(ROOT/'module/template',mod)
    for target,source in [('payload/permissions','etc/permissions'),('payload/default-permissions','etc/default-permissions'),('metadata-template/vintf','etc/vintf/manifest')]:
        shutil.copytree(system_ext/source,mod/target,dirs_exist_ok=True)
    checks.baseline(system_ext/'etc/permissions',mod/'baseline/permissions')
    checks.baseline(system_ext/'etc/default-permissions',mod/'baseline/default-permissions')
    checks.baseline(system_ext/'etc/vintf/manifest',mod/'metadata-template/baseline/vintf')
    checks.baseline(mod/'metadata-template/vintf',mod/'metadata-template/candidate-vintf')
    ctx=system_ext/'etc/selinux/system_ext_service_contexts'
    put(mod/'baseline/service-contexts.sha256',digest(ctx)+'\n')
    put(mod/'payload/system_ext_service_contexts',ctx.read_text().rstrip('\n')+'\n'+checks.CONTEXT_LINE+'\n')
    clean={k:v for k,v in profile.items() if k not in ['system_ext_root','app_apk']}
    env={'PROFILE_DEVICE':profile['device'],'PROFILE_SLOT':profile['slot_suffix'],'PROFILE_KERNEL':profile['kernel_release'],'PROFILE_FINGERPRINT':profile['fingerprint'],'PROFILE_INCREMENTAL':profile['vendor_dlkm_incremental'],'PROFILE_KSUD_SHA':profile['ksud_sha256'],'PROFILE_GOOGLE_SHA':checks.GOOGLE_SHA,'PROFILE_APP_SHA':profile['app_sha256']}
    for folder in [mod,mod/'metadata-template']:
        put(folder/'profile.json',json.dumps(clean,indent=2)+'\n')
        put(folder/'profile.env',''.join(k+'='+shlex.quote(v)+'\n' for k,v in env.items()))
    put(mod/'profile-device.sha256',''.join(profile[k]+'  '+v+'\n' for k,v in checks.DEVICE_PATHS.items()))
    for name,value in [('expected-fingerprint.txt',profile['fingerprint']),('expected-ksud.sha256',profile['ksud_sha256']),('expected-apk.sha256',checks.GOOGLE_SHA)]:put(mod/'metadata-template'/name,value+'\n')
    for rel in ['state-classes.dex','auto/observer.dex','metadata-template/bind-ro']:
        p=mod/rel;p.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(inputs['module/'+rel],p)
    p=mod/'payload/priv-app/MoseyApp/MoseyApp.apk';p.parent.mkdir(parents=True);shutil.copyfile(inputs['MoseyApp.apk'],p)
    shutil.copyfile(app,mod/'companion.apk')
    rollback={'finalize.sh':mod/'installer/finalize.sh','finalize-launcher.sh':mod/'installer/finalize-launcher.sh','common.sh':mod/'common.sh','state-classes.dex':mod/'state-classes.dex','country-lease.sh':mod/'country-lease.sh'}
    put(mod/'installer/rollback-assets.sha256',''.join(digest(p)+'  ./'+n+'\n' for n,p in sorted(rollback.items())))
    manifest(mod/'auto',mod/'auto/auto-assets.sha256',['auto-assets.sha256','enabled'])
    manifest(mod/'metadata-template',mod/'metadata-template/early-assets.sha256',['early-assets.sha256'])
    manifest(mod,mod/'module-assets.sha256',['module-assets.sha256','installation-assets.sha256','customize.sh'])
    manifest(mod,mod/'installation-assets.sha256',['installation-assets.sha256'])
    zip_tree(mod,out/'KSU-module.zip')
    shutil.copyfile(app,out/'companion.apk')
    report={'version':'0.5.1-preview.1','assembly_only':True,'new_build_phone_validated':False,'reference_version_phone_validated':True,'profile':profile['profile_id'],'files':{n:digest(out/n) for n in ['KSU-module.zip','companion.apk']}}
    put(out/'BUILD-RESULT.json',json.dumps(report,indent=2)+'\n')
    return report
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--open-runtime',type=Path,required=True);p.add_argument('--dependencies',type=Path,required=True)
    p.add_argument('--system-ext',type=Path,required=True);p.add_argument('--output',type=Path,default=ROOT/'build/local-release')
    p.add_argument('--sdk',type=Path,default=os.environ.get('ANDROID_SDK_ROOT',os.environ.get('ANDROID_HOME')))
    p.add_argument('--jdk',type=Path,default=os.environ.get('JAVA_HOME'))
    p.add_argument('--check-only',action='store_true')
    args=p.parse_args();profile=json.loads((ROOT/'profiles/pixelos-houji-20261021-0252-b.json').read_text())
    inputs=resolve_inputs(args.open_runtime.resolve(),args.dependencies.resolve())
    for name,rel in checks.SYSTEM_DIRS.items():
        if checks.tree_digest(args.system_ext/rel)!=profile['system_ext_checks'][name]:raise ValueError('Original ROM directory mismatch: '+name)
    if digest(args.system_ext/'etc/selinux/system_ext_service_contexts')!=profile['system_ext_checks']['service_contexts']:raise ValueError('Original ROM contexts mismatch')
    if args.check_only:print('INPUTS_VERIFIED');return
    if not args.sdk or not args.jdk:p.error('Set ANDROID_SDK_ROOT and JAVA_HOME (JDK root, not bin) or pass --sdk and --jdk')
    if args.output.exists():raise ValueError('Output already exists; choose a fresh directory')
    args.output.mkdir(parents=True)
    app=build_app(inputs,args.output/'app-build',args.sdk.resolve(),args.jdk.resolve()/'bin')
    print(json.dumps(build_module(profile,args.system_ext.resolve(),app,inputs,args.output),indent=2))
if __name__=='__main__':main()
