"""Compile public Java runtime helpers using JDK 17 and Android SDK 36."""
from pathlib import Path
import argparse,json,os
from assemble import ROOT,run,tool,digest
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--sdk',type=Path,default=os.environ.get('ANDROID_SDK_ROOT',os.environ.get('ANDROID_HOME')))
p.add_argument('--jdk',type=Path,default=os.environ.get('JAVA_HOME'))
p.add_argument('--output',type=Path,default=ROOT/'build/java-runtime')
a=p.parse_args()
if not a.sdk or not a.jdk:p.error('Set ANDROID_SDK_ROOT and JAVA_HOME')
a.output.mkdir(parents=True,exist_ok=False)
bt=a.sdk/'build-tools/36.0.0';android=a.sdk/'platforms/android-36/android.jar';jdk=a.jdk/'bin'
groups={
 'quickshare-runtime/qs-classes.dex':list((ROOT/'native/control').glob('*.java')),
 'runtime/ble-classes.dex':[ROOT/'native/ble/BleWakeProbe.java'],
 'module/state-classes.dex':[ROOT/'module/java/IntegrationState.java'],
 'module/auto/observer.dex':[ROOT/'observer'/x for x in ['RootForeground.java','EntryGate.java','RootQuickShareObserver.java']],
}
spec=json.loads((ROOT/'profiles/runtime-inputs.json').read_text());result={}
for i,(target,files) in enumerate(groups.items()):
    c=a.output/str(i)/'classes';d=a.output/str(i)/'dex';c.mkdir(parents=True);d.mkdir()
    run([tool(jdk/'javac'),'--release','8','-encoding','UTF-8','-cp',android,'-d',c,*files])
    run([tool(jdk/'java'),'-cp',bt/'lib/d8.jar','com.android.tools.r8.D8','--min-api','36','--lib',android,'--output',d,*sorted(c.glob('*.class'))])
    dst=a.output/target;dst.parent.mkdir(parents=True,exist_ok=True);dst.write_bytes((d/'classes.dex').read_bytes())
    result[target]={'sha256':digest(dst),'matches_reference':digest(dst)==spec[target]['sha256']}
test=a.output/'test';test.mkdir()
run([tool(jdk/'javac'),'--release','8','-d',test,ROOT/'observer/EntryGate.java',ROOT/'observer/EntryGateTest.java'])
print(run([tool(jdk/'java'),'-cp',test,'EntryGateTest']).strip())
(a.output/'comparison.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
