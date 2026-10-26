package org.houji.airdrop;

import android.app.*;
import android.content.*;
import android.os.*;
import android.net.Uri;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.UUID;

public final class ShareService extends Service {
    static volatile boolean running;
    private PowerManager.WakeLock wake;
    private File job;
    public IBinder onBind(Intent i){return null;}
    public void onCreate(){super.onCreate();getSystemService(NotificationManager.class).createNotificationChannel(new NotificationChannel("transfer","文件发送",NotificationManager.IMPORTANCE_LOW));}
    private Notification notification(String text){return new Notification.Builder(this,"transfer").setSmallIcon(android.R.drawable.stat_sys_upload).setContentTitle("AirDrop 实验分享").setContentText(text).setOngoing(true).setContentIntent(PendingIntent.getActivity(this,0,new Intent(this,MainActivity.class),PendingIntent.FLAG_IMMUTABLE)).build();}
    private void status(String s){try{AppFiles.write(new File(getFilesDir(),"ui-status.txt"),s);}catch(Exception ignored){}getSystemService(NotificationManager.class).notify(1,notification(s.length()>80?s.substring(0,80):s));}
    public int onStartCommand(Intent i,int flags,int id){
        synchronized(ShareService.class){
            if(running)return START_NOT_STICKY;
            if(QuickShareService.running){stopSelf();return START_NOT_STICKY;}
            running=true;
        }
        startForeground(1,notification("准备发送"));
        wake=((PowerManager)getSystemService(POWER_SERVICE)).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,"houji:share");wake.acquire(20*60*1000L);
        Uri uri=i==null?null:i.getData();String name=i==null?null:i.getStringExtra("filename");
        new Thread(()->execute(uri,name),"airdrop-share").start();return START_NOT_STICKY;
    }
    private void execute(Uri uri,String name){
        boolean uploadComplete=false,recoveredSameBoot=false,rebootRequested=false,cancelled=false;
        try {
            if(uri==null||!"content".equals(uri.getScheme()))throw new IOException("没有可读取的文件");
            job=new File(getFilesDir(),"jobs/"+UUID.randomUUID());if(!job.mkdirs())throw new IOException("Cannot create private job");
            AppFiles.write(new File(job,"session-kind"),"direct\n");
            AppFiles.write(new File(getFilesDir(),"current-run.txt"),job.getAbsolutePath());
            status("正在准备文件与运行组件…");File base=AppFiles.runtime(this);
            File identity=new File(getFilesDir(),"identity.p12");if(!identity.isFile())throw new IOException("缺少本机测试身份，请先完成初始化");
            AppFiles.copy(identity,new File(job,"identity.p12"));
            long count=0,last=0;MessageDigest digest=MessageDigest.getInstance("SHA-256");File source=new File(job,"source.bin");
            try(InputStream in=getContentResolver().openInputStream(uri);OutputStream out=new FileOutputStream(source)) {
                if(in==null)throw new IOException("Cannot open selected file");byte[] b=new byte[128*1024];int n;
                while((n=in.read(b))!=-1){if(new File(job,"cancel").exists())throw new IOException("已取消，手机设置未改变");count+=n;if(count>AppFiles.MAX_SOURCE)throw new IOException("当前实验版单文件上限 4 GiB");out.write(b,0,n);digest.update(b,0,n);
                    if(count-last>=8*1024*1024){status("正在准备文件："+android.text.format.Formatter.formatFileSize(this,count));last=count;}}
            }
            if(count==0)throw new IOException("暂不支持空文件");source.setWritable(false,true);
            name=AppFiles.name(name);AppFiles.payload(job,name,getContentResolver().getType(uri),count,AppFiles.hex(digest.digest()));
            status("等待 KSU 授权 / 启动分享。请只授权本应用。\n文件："+name+"\n结束后自动恢复 Wi-Fi；恢复异常时会自动重启。");
            String cmd="/system/bin/sh "+AppFiles.quote(new File(base,"phone-runner.sh").getAbsolutePath())+" "+AppFiles.quote(base.getAbsolutePath())+" "+AppFiles.quote(job.getAbsolutePath());
            java.lang.Process process=new ProcessBuilder("su","-c",cmd).redirectErrorStream(true).start();
            try(BufferedReader reader=new BufferedReader(new InputStreamReader(process.getInputStream(),StandardCharsets.UTF_8));Writer log=new OutputStreamWriter(new FileOutputStream(new File(job,"app-runner.log")),StandardCharsets.UTF_8)) {
                String line;while((line=reader.readLine())!=null){log.write(line+"\n");log.flush();
                    if(line.contains("ask_waiting_for_user"))status("请在 iPhone 上点“接受”。\n文件："+name);
                    else if(line.contains("upload_progress_bytes="))status("正在发送："+line);
                    else if(line.equals("upload_http_success=true")){uploadComplete=true;status("上传已完成，请在 iPhone 检查文件。\n正在恢复普通 Wi-Fi，请稍候。");}
                    else if(line.equals("MOBILE_CANCELLED=true")){cancelled=true;status("已取消发送，正在恢复普通 Wi-Fi。");}
                    else if(line.startsWith("MOBILE_PHASE=")){
                        String phase=line.substring(13),label=phase;
                        if(phase.equals("PRECHECK"))label="正在检查当前系统与无线状态";
                        else if(phase.equals("PRECHECK_FAILED"))label="检查未通过，尚未切换无线组件";
                        else if(phase.equals("CONTROLS_STOPPING")||phase.equals("NORMAL_UNLOAD")||phase.equals("CANDIDATE_LOADING"))label="正在临时切换无线组件";
                        else if(phase.equals("SESSION_STARTING")||phase.equals("DISCOVERING_AND_SENDING"))label="正在寻找 iPhone，请保持对方亮屏并开启 AirDrop";
                        else if(phase.equals("SESSION_FINISHED")||phase.equals("NATIVE_THREADS_STOPPING")||phase.equals("WONDER_UNLOADING")||phase.equals("PROVIDER_RELEASING")||phase.equals("CANDIDATE_UNLOADING")||phase.equals("C2_RELOADING"))label="会话结束，正在自动恢复普通 Wi-Fi，请稍候";
                        else if(phase.equals("RECOVERED_C2_SAME_BOOT")){recoveredSameBoot=true;label="普通 Wi-Fi 已自动恢复，无需重启；正在结束本轮任务";}
                        else if(phase.equals("RECOVERY_REBOOT_REQUESTED")){rebootRequested=true;label="自动恢复未完成，正在重启恢复普通 Wi-Fi";}
                        status(label);
                    }
                    else if(line.startsWith("ERROR")||line.startsWith("error=")||line.startsWith("MOBILE_ERROR=")||line.startsWith("MOBILE_RUN_ERROR="))status(line+"\n请等待本轮退出并检查状态。");
                }
            }
            int code=process.waitFor();cancelled=cancelled||new File(job,"cancel").exists();
            String transfer=uploadComplete?"上传已完成，请在 iPhone 检查文件。":cancelled?"本轮已取消发送。":"未确认文件上传完成。";
            if(recoveredSameBoot)status(transfer+"\n普通 Wi-Fi 已自动恢复，无需重启。"+(code!=0&&!cancelled?"\n发送进程退出码："+code:""));
            else if(rebootRequested)status(transfer+"\n正在重启恢复普通 Wi-Fi，请保持手机开机。");
            else status(transfer+"\n发送进程结束（"+code+"）。\n"+(new File(job,"device-changed.value").exists()?"尚未确认恢复完成，请点击“检查 Root / 上次恢复状态”；如无线异常，完整重启小米。":"尚未修改无线状态，无需为此重启。")+"\n"+AppFiles.read(new File(job,"app-runner.log"),3500));
        }catch(Exception error){status("无法开始 / 发送失败："+error.getMessage()+(recoveredSameBoot?"\n普通 Wi-Fi 已自动恢复，无需重启。":""));}
        finally{running=false;if(wake!=null&&wake.isHeld())wake.release();stopForeground(STOP_FOREGROUND_REMOVE);stopSelf();}
    }
}
