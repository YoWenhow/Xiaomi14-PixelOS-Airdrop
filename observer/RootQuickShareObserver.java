import android.content.pm.PackageInfo;
import android.os.SystemClock;
import android.system.Os;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.UUID;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

/** A foreground entry detector. It never loads drivers or kills a radio process. */
public final class RootQuickShareObserver {
  private static final String APP="org.houji.airdrop";
  private static final File MOD=new File("/data/adb/modules/houji_qs_system79");
  private static final File META=new File("/metadata/houji-qs-system79");
  private static final File AUTO=new File(MOD,"auto");
  private static final File RAM=new File("/dev/houji-qs-auto");
  private static final File APPFILES=new File("/data/user/0/"+APP+"/files");
  private static final File BASE=new File(APPFILES,"quickshare-runtime");
  private static final File LOCK=new File("/dev/.houji-airdrop-mobile.lock");
  private static final File MANAGEMENT=new File("/dev/houji-qs-system79-management-lock");
  private final RootForeground foreground;
  private final String boot;
  private int appUid;
  private String token;
  private File job;
  private boolean cancelAcknowledged;
  private long launchAt,lastCancelAttempt;
  private boolean stop;

  private RootQuickShareObserver() throws Exception {
    foreground=new RootForeground();boot=read(new File("/proc/sys/kernel/random/boot_id"),100).trim();
    if(!boot.matches("[0-9a-f-]{36}"))throw new IOException("BOOT_ID");
    if(!RAM.isDirectory()||Os.stat(RAM.getPath()).st_uid!=0)throw new IOException("OBSERVER_RAM_OWNER");
    packageCheck();
    write(new File(RAM,"pid"),Integer.toString(android.os.Process.myPid())+"\n");
    write(new File(RAM,"starttime"),processStart(android.os.Process.myPid())+"\n");
    write(new File(RAM,"boot"),boot+"\n");
    write(new File(RAM,"ready"),"1\n");
  }
  private static String read(File f,int max){
    try(InputStream in=new FileInputStream(f);ByteArrayOutputStream out=new ByteArrayOutputStream()){
      byte[] b=new byte[1024];int n;while(out.size()<max&&(n=in.read(b,0,Math.min(b.length,max-out.size())))!=-1)out.write(b,0,n);
      return new String(out.toByteArray(),StandardCharsets.UTF_8);
    }catch(IOException e){return "";}
  }
  private static void write(File f,String value)throws IOException{
    try(OutputStream out=new FileOutputStream(f)){out.write(value.getBytes(StandardCharsets.UTF_8));}
  }
  private static String processStart(int pid)throws IOException{
    String s=read(new File("/proc/"+pid+"/stat"),4096);int end=s.lastIndexOf(") ");
    if(end<0)throw new IOException("PROC_STAT");String[] fields=s.substring(end+2).trim().split("\\s+");
    if(fields.length<=19||!fields[19].matches("[0-9]+"))throw new IOException("PROC_STARTTIME");return fields[19];
  }
  private void packageCheck()throws Exception{
    PackageInfo pi=foreground.context.getPackageManager().getPackageInfo(APP,0);
    if(pi.getLongVersionCode()<9)throw new IOException("APP_VERSION_TOO_OLD");
    appUid=pi.applicationInfo.uid;if(appUid<10000||appUid>=100000)throw new IOException("APP_USER_NOT_ZERO");
  }
  private boolean enabled(){
    return new File(AUTO,"enabled").isFile()&&!new File(RAM,"stop").exists()
      &&new File(META,"ENABLED").isFile()&&!new File(META,"DISABLED").exists()
      &&!new File(MOD,"disable").exists()&&boot.equals(read(new File(META,"boot-ready.boot_id"),100).trim())
      &&boot.equals(read(new File(META,"postfs-success.boot_id"),100).trim());
  }
  private static void event(String text){System.out.println("AUTO_EVENT="+text);System.out.flush();}
  private static final class Result {int code;String output;Result(int c,String s){code=c;output=s;}}
  private static Result command(String... args)throws Exception{
    Process p=new ProcessBuilder(args).redirectErrorStream(true).start();p.getOutputStream().close();
    ByteArrayOutputStream out=new ByteArrayOutputStream();
    Thread drain=new Thread(()->{try(InputStream in=p.getInputStream()){byte[] b=new byte[1024];int n;while((n=in.read(b))!=-1){synchronized(out){if(out.size()<16384)out.write(b,0,Math.min(n,16384-out.size()));}}}catch(IOException ignored){}},"auto-command-output");
    drain.setDaemon(true);drain.start();
    if(!p.waitFor(15,TimeUnit.SECONDS)){p.destroy();return new Result(124,"TIMEOUT");}
    drain.join(1000);synchronized(out){return new Result(p.exitValue(),new String(out.toByteArray(),StandardCharsets.UTF_8));}
  }
  private void startSession()throws Exception{
    packageCheck();
    token=UUID.randomUUID().toString();job=null;cancelAcknowledged=false;launchAt=SystemClock.elapsedRealtime();lastCancelAttempt=0;
    Result r=command("/system/bin/am","start-foreground-service","--user","0","-n",APP+"/.QuickShareService","--ez","automatic","true","--es","auto_token",token);
    if(r.code!=0||!r.output.contains("Starting service:")||r.output.contains("Error:")){event("START_REJECTED_rc_"+r.code);token=null;return;}
    event("START_REQUESTED");
  }
  private File matchingJob()throws Exception{
    if(token==null)return null;
    String path=read(new File(APPFILES,"current-run.txt"),1024).trim();
    String prefix=new File(APPFILES,"jobs").getPath()+"/";
    if(!path.startsWith(prefix))return null;
    String id=path.substring(prefix.length());
    if(!id.matches("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}"))return null;
    File f=new File(path);String real=f.getCanonicalPath();
    if(!real.equals(path)&&!real.equals("/data/data/"+APP+"/files/jobs/"+id))return null;
    if(!f.isDirectory()||Os.stat(path).st_uid!=appUid)return null;
    if(!token.equals(read(new File(f,"automatic-token"),100).trim())||!"automatic".equals(read(new File(f,"direction"),32).trim()))return null;
    return f;
  }
  private void updateJob(long now)throws Exception{
    if(token==null)return;
    if(job==null){job=matchingJob();if(job!=null)event("JOB_ATTACHED");}
    if(job==null){if(now-launchAt>60000){event("NO_MATCHING_JOB");token=null;}return;}
    if(!LOCK.exists()){
      try{
        JSONObject state=new JSONObject(read(new File(job,"status.json"),8192));
        String phase=state.optString("phase");
        if(state.optBoolean("recovery_verified")&&(phase.equals("RECOVERED_C2_SAME_BOOT")||phase.equals("RECOVERED_C2"))){event("RECOVERED");token=null;job=null;return;}
      }catch(Exception ignored){}
      if(now-launchAt>60000&&!new File(job,"device-changed.value").exists()){event("PREPARATION_ENDED");token=null;job=null;}
    }
  }
  private void idleCancel(long now)throws Exception{
    if(token==null||job==null||cancelAcknowledged||now-lastCancelAttempt<2500)return;
    if(!job.getPath().equals(read(new File(LOCK,"quickshare-job"),1024).trim()))return;
    lastCancelAttempt=now;
    Result r=command("/system/bin/sh",new File(BASE,"quickshare-runner.sh").getPath(),"--cancel-if-idle",BASE.getPath(),job.getPath());
    if(r.code==0&&r.output.contains("MOBILE_IDLE_CANCEL_REQUESTED=true")){cancelAcknowledged=true;event("IDLE_CANCEL_REQUESTED");}
    else event("IDLE_CANCEL_RETRY_rc_"+r.code);
  }
  private void run()throws Exception{
    int errors=0,ticks=0;EntryGate gate=new EntryGate();
    event("READY");
    while(!stop&&enabled()){
      long now=SystemClock.elapsedRealtime();
      if(++ticks%10==0)packageCheck(); // A downgrade/removal stops the observer, not just one sample.
      try{
        updateJob(now);RootForeground.Snapshot s=foreground.snapshot();errors=0;
        int action=gate.update(s.quickShare,s.interactive&&!s.locked&&s.currentUser==0,token!=null||LOCK.exists()||MANAGEMENT.exists(),token!=null,now);
        if(action==EntryGate.START&&enabled()&&!LOCK.exists()&&!MANAGEMENT.exists())startSession();
        else if(action==EntryGate.CANCEL_IF_IDLE)idleCancel(now);
      }catch(Exception e){
        errors++;gate.invalid();event("OBSERVATION_ERROR_"+e.getClass().getSimpleName());
        if(errors>=5)throw e;
      }
      Thread.sleep(1000);
    }
    // Stopping the observer never force-cancels a real transfer.
    idleCancel(SystemClock.elapsedRealtime()+3000);event("STOPPED");
  }
  public static void main(String[] args){
    try{
      RootQuickShareObserver o=new RootQuickShareObserver();
      Runtime.getRuntime().addShutdownHook(new Thread(()->{o.stop=true;try{o.idleCancel(SystemClock.elapsedRealtime()+3000);}catch(Exception ignored){}},"auto-shutdown"));
      o.run();System.exit(0);
    }catch(Throwable t){event("FATAL_"+t.getClass().getSimpleName());t.printStackTrace();System.exit(1);}
  }
}
