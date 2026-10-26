package org.houji.airdrop;

import android.app.*;
import android.content.*;
import android.net.Uri;
import android.os.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.UUID;

/** One Google sharing session per fresh radio instance; the root controller owns recovery. */
public final class QuickShareService extends Service {
    static volatile boolean running;
    private static volatile boolean ready,launched,cancelRequested,rootStarted;
    private static volatile File job,base;
    private static File cancelWorkerJob;
    private static Intent sharingIntent;
    private PowerManager.WakeLock wake;
    public IBinder onBind(Intent i){return null;}
    public void onCreate(){super.onCreate();getSystemService(NotificationManager.class).createNotificationChannel(new NotificationChannel("quickshare","Quick Share 会话",NotificationManager.IMPORTANCE_LOW));}
    private Notification notification(String text){
        return new Notification.Builder(this,"quickshare").setSmallIcon(android.R.drawable.stat_sys_upload)
            .setContentTitle("AirDrop · Quick Share").setContentText(text).setOngoing(true)
            .setContentIntent(PendingIntent.getActivity(this,79,new Intent(this,MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),PendingIntent.FLAG_IMMUTABLE|PendingIntent.FLAG_UPDATE_CURRENT)).build();
    }
    private void status(String text){try{AppFiles.write(new File(getFilesDir(),"ui-status.txt"),text);}catch(IOException ignored){}
        getSystemService(NotificationManager.class).notify(79,notification(text.length()>90?text.substring(0,90):text));}
    // Launch only from the resumed activity: Android blocks background activity launches.
    static synchronized Intent takeLaunchIntent(){
        if(!running||!ready||launched||cancelRequested||sharingIntent==null)return null;
        launched=true;return new Intent(sharingIntent);
    }
    public int onStartCommand(Intent request,int flags,int startId){
        synchronized(ShareService.class){
            if(running)return START_NOT_STICKY;
            if(ShareService.running){stopSelf();return START_NOT_STICKY;}
            running=true;
        }
        ready=false;launched=false;cancelRequested=false;rootStarted=false;job=null;base=null;sharingIntent=null;
        startForeground(79,notification("准备 Quick Share"));
        wake=getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,"houji:quickshare");wake.acquire(42*60*1000L);
        new Thread(()->execute(request),"quickshare-session").start();return START_NOT_STICKY;
    }
    static synchronized void requestCancel(){
        cancelRequested=true;
        if(!rootStarted||base==null||job==null)return;
        final File b=base,j=job;
        if(cancelWorkerJob==j)return;cancelWorkerJob=j;
        new Thread(()->{try{
            long deadline=SystemClock.elapsedRealtime()+60000;
            // su may not yet have created the radio lock: retry until root acknowledges this job.
            while(cancelRequested&&running&&rootStarted&&job==j&&SystemClock.elapsedRealtime()<deadline){
                java.lang.Process p=new ProcessBuilder("su","-c",command(b,j,"--cancel")).redirectErrorStream(true).start();
                if(!p.waitFor(5,java.util.concurrent.TimeUnit.SECONDS)){p.destroy();}
                else {StringBuilder out=new StringBuilder();try(BufferedReader reader=new BufferedReader(new InputStreamReader(p.getInputStream()))){String s;while((s=reader.readLine())!=null&&out.length()<4096)out.append(s).append('\n');}
                    if(p.exitValue()==0&&out.indexOf("MOBILE_CANCEL_REQUESTED=true")>=0)return;
                }
                Thread.sleep(500);
            }
        }catch(Exception ignored){}finally{synchronized(QuickShareService.class){if(cancelWorkerJob==j)cancelWorkerJob=null;}}},"quickshare-cancel").start();
    }
    static String command(File b,File j,String mode){return "/system/bin/sh "+AppFiles.quote(new File(b,"quickshare-runner.sh").getAbsolutePath())+" "+mode+" "+AppFiles.quote(b.getAbsolutePath())+" "+AppFiles.quote(j.getAbsolutePath());}
    private void execute(Intent request){
        boolean recovered=false,reboot=false,completed=false,cancelled=false;
        try{
            if(request==null)throw new IOException("没有待启动的会话");
            boolean automatic=request.getBooleanExtra("automatic",false),receive=request.getBooleanExtra("receive",false);Uri uri=request.getData();
            String autoToken=request.getStringExtra("auto_token");
            if(automatic&&(autoToken==null||!autoToken.matches("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")))throw new IOException("自动启动凭据无效");
            Intent target=null;
            if(automatic){ /* GMS already owns the original URI grants and foreground share page. */ }
            else if(receive)target=new Intent("com.google.android.gms.RECEIVE_NEARBY");
            else{
                if(uri==null||!"content".equals(uri.getScheme()))throw new IOException("请选择可读取的文件");
                try(InputStream in=getContentResolver().openInputStream(uri)){if(in==null)throw new IOException("文件不可读取");}
                String mime=getContentResolver().getType(uri);
                target=new Intent(Intent.ACTION_SEND).setType(mime==null?"*/*":mime).putExtra(Intent.EXTRA_STREAM,uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                target.setClipData(ClipData.newRawUri("shared file",uri));
            }
            if(target!=null)target.setComponent(new ComponentName("com.google.android.gms","com.google.android.gms.nearby.sharing.main.MainActivity"));
            synchronized(QuickShareService.class){sharingIntent=target;}
            job=new File(getFilesDir(),"jobs/"+UUID.randomUUID());if(!job.mkdirs())throw new IOException("无法创建会话目录");
            AppFiles.write(new File(job,"session-kind"),"quickshare\n");AppFiles.write(new File(job,"direction"),automatic?"automatic\n":receive?"receive\n":"send\n");
            if(automatic)AppFiles.write(new File(job,"automatic-token"),autoToken+"\n");
            AppFiles.write(new File(getFilesDir(),"current-run.txt"),job.getAbsolutePath());
            status(automatic?"检测到系统 Quick Share，正在准备 iPhone 互传；请保持分享页面，稍候即可发现设备。":"正在准备 Quick Share；请稍候，准备好后将自动打开系统分享页。");
            base=AppFiles.runtime(this,"quickshare-runtime");
            if(cancelRequested){cancelled=true;throw new IOException("已取消，尚未切换无线组件");}
            java.lang.Process p=new ProcessBuilder("su","-c",command(base,job,"")).redirectErrorStream(true).start();rootStarted=true;
            if(cancelRequested)requestCancel();
            try(BufferedReader r=new BufferedReader(new InputStreamReader(p.getInputStream(),StandardCharsets.UTF_8));Writer log=new OutputStreamWriter(new FileOutputStream(new File(job,"app-runner.log")),StandardCharsets.UTF_8)){
                String line;while((line=r.readLine())!=null){log.write(line+"\n");log.flush();
                    if(line.equals("QUICKSHARE_READY=1")){ready=true;status(automatic?"iPhone 互传已准备好。请继续在系统 Quick Share 中选择设备或接受文件；完成后自动恢复 Wi-Fi。":receive?"已准备接收。请在 iPhone 的 AirDrop 中选择小米，再在小米上接受。\n若未自动打开接收页，请点此通知回到应用。":"已准备发送。请在 Quick Share 中选择 iPhone，并在 iPhone 接受。\n若未自动打开分享页，请点此通知回到应用。");}
                    else if(line.equals("MOBILE_CANCELLED=true")){cancelled=true;ready=false;status("正在取消会话并恢复普通 Wi-Fi，请稍候。");}
                    else if(line.equals("MOBILE_PROVIDER_STOPPED=true")){ready=false;status("系统分享服务已停止，本轮未完成，正在自动恢复普通 Wi-Fi。");}
                    else if(line.equals("QUICKSHARE_TRANSFER_COMPLETE=1")){completed=true;ready=false;status("Quick Share 已完成本轮传输，正在恢复普通 Wi-Fi。");}
                    else if(line.startsWith("MOBILE_PHASE=")){
                        String phase=line.substring(13);
                        if(phase.equals("RECOVERED_C2_SAME_BOOT")){recovered=true;ready=false;status("普通 Wi-Fi 已恢复，正在结束本轮会话。");}
                        else if(phase.equals("RECOVERY_REBOOT_REQUESTED")){reboot=true;ready=false;status("自动恢复未完成，正在重启恢复普通 Wi-Fi。");}
                        else if(phase.equals("PRECHECK"))status("正在检查 Root、系统接入与无线状态…");
                        else if(phase.equals("PRECHECK_FAILED"))status("准备检查未通过，请查看下方结果。");
                        else if(phase.equals("QUICKSHARE_SETTLING")){ready=false;status("数据传输已结束，正在等待系统完成收尾，请稍候。");}
                        else if(phase.equals("CONTROLS_STOPPING")||phase.equals("NORMAL_UNLOAD")||phase.equals("CANDIDATE_LOADING"))status("正在切换到 AirDrop 分享，请稍候…");
                        else if(phase.equals("SESSION_FINISHED")||phase.equals("NATIVE_THREADS_STOPPING")||phase.equals("WONDER_UNLOADING")||phase.equals("PROVIDER_RELEASING")||phase.equals("CANDIDATE_UNLOADING")||phase.equals("C2_RELOADING")){ready=false;status("会话结束，正在恢复普通 Wi-Fi。恢复完成后可再次发送或接收。");}
                    }else if(line.startsWith("MOBILE_ERROR=")||line.startsWith("MOBILE_RUN_ERROR="))status("本轮遇到问题，正在退出并检查恢复。\n"+line);
                }
            }
            int code=p.waitFor();rootStarted=false;
            String outcome=completed?"Quick Share 已报告传输完成，请在接收端检查文件。":cancelled||cancelRequested?"本轮已取消。":"本轮已结束；未确认文件传输成功。";
            if(recovered)status(outcome+"\n普通 Wi-Fi 已恢复，无需重启。可开始下一轮发送或接收。"+(code!=0&&!cancelled?"\n退出码："+code:""));
            else if(reboot)status(outcome+"\n正在重启恢复，请保持手机开机。");
            else status(outcome+"\n退出码："+code+"。请点击“检查 Root / 恢复状态”。\n"+AppFiles.read(new File(job,"app-runner.log"),4000));
        }catch(Exception e){status("无法完成本轮："+e.getMessage()+"\n如已进入分享，请等待自动恢复；无线异常时完整重启小米。");}
        finally{ready=false;sharingIntent=null;rootStarted=false;running=false;if(wake!=null&&wake.isHeld())wake.release();stopForeground(STOP_FOREGROUND_REMOVE);stopSelf();}
    }
}
