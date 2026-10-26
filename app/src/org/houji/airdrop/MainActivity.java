package org.houji.airdrop;

import android.app.*;
import android.os.*;
import android.content.*;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.view.*;
import android.widget.*;
import java.io.File;

public final class MainActivity extends Activity {
    private Uri selected;private String filename;private TextView chosen,status;private Button send,receiveButton,direct,choose;private volatile boolean checking;
    private boolean busy(){return ShareService.running||QuickShareService.running;}
    private final Handler handler=new Handler();
    private final Runnable refresh=new Runnable(){public void run(){
        String text=AppFiles.read(new File(getFilesDir(),"ui-status.txt"),8192);
        if(!text.isEmpty())status.setText(text);
        boolean idle=!busy()&&!checking&&!AppFiles.recoveryPending(MainActivity.this);
        if(!busy()&&!checking&&AppFiles.recoveryPending(MainActivity.this))status.setText(text+"\n上轮恢复尚未确认，请先点击“检查 Root / 恢复状态”。");
        send.setEnabled(selected!=null&&idle);direct.setEnabled(selected!=null&&idle);receiveButton.setEnabled(idle);choose.setEnabled(idle);
        Intent launch=QuickShareService.takeLaunchIntent();
        if(launch!=null){try{startActivity(launch);}catch(Exception e){QuickShareService.requestCancel();setStatus("无法打开系统 Quick Share，正在恢复："+e.getMessage());}}
        handler.postDelayed(this,1000);
    }};
    public void onCreate(Bundle state) {
        super.onCreate(state);getWindow().setStatusBarColor(0xff126c70);
        LinearLayout box=new LinearLayout(this);box.setOrientation(1);box.setPadding(30,48,30,32);
        ScrollView sc=new ScrollView(this);sc.addView(box);setContentView(sc);
        sc.setOnApplyWindowInsetsListener((v,insets)->{android.graphics.Insets bars=insets.getInsets(WindowInsets.Type.systemBars());v.setPadding(bars.left,bars.top,bars.right,bars.bottom);return insets;});
        TextView title=new TextView(this);title.setText("Quick Share 自动接入");title.setTextSize(27);title.setTextColor(0xff126c70);box.addView(title);
        TextView info=new TextView(this);info.setText("开启自动接入后，直接从系统分享菜单选择 Quick Share，或打开其接收页，即可自动准备 iPhone 互传。无需先打开这个应用。\n\niPhone 需开启“所有人，10 分钟”并解锁。准备期间普通 Wi-Fi 暂停，完成后自动恢复。未开始传输时退出分享页也会恢复；已开始传输可在后台继续。\n\n等待设备最多 3 分钟；文件传输最多 30 分钟。异常恢复仍可能重启。每轮完成后退出并重新打开 Quick Share，即可开始下一轮。下方保留手动入口。");info.setTextSize(16);info.setPadding(0,22,0,20);box.addView(info);
        Button autoEnable=new Button(this);autoEnable.setText("开启系统 Quick Share 自动接入");autoEnable.setOnClickListener(v->changeAutomatic("enable"));box.addView(autoEnable);
        Button autoDisable=new Button(this);autoDisable.setText("暂停自动接入");autoDisable.setOnClickListener(v->changeAutomatic("disable"));box.addView(autoDisable);
        chosen=new TextView(this);chosen.setText("尚未选择文件");chosen.setTextSize(17);box.addView(chosen);
        choose=new Button(this);choose.setText("选择文件");choose.setOnClickListener(v->{Intent i=new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*").addCategory(Intent.CATEGORY_OPENABLE);startActivityForResult(i,1);});box.addView(choose);
        send=new Button(this);send.setText("通过 Quick Share 发送");send.setEnabled(false);send.setOnClickListener(v->startQuickShare(false));box.addView(send);
        receiveButton=new Button(this);receiveButton.setText("接收 iPhone 文件");receiveButton.setOnClickListener(v->startQuickShare(true));box.addView(receiveButton);
        Button cancel=new Button(this);cancel.setText("结束会话并恢复 Wi-Fi");cancel.setOnClickListener(v->{
            if(QuickShareService.running){QuickShareService.requestCancel();setStatus("已请求结束；正在停止并恢复，请保持手机开机。");return;}
            if(!ShareService.running){setStatus("当前没有应用会话；如需核对上轮恢复，请点击检查。");return;}
            try{File job=priorJob();if(job!=null){AppFiles.write(new File(job,"cancel"),"cancel\n");setStatus("已请求取消；正在停止并恢复，请保持手机开机。");}}catch(Exception e){setStatus(e.toString());}
        });box.addView(cancel);
        Button check=new Button(this);check.setText("检查 Root / 恢复状态");check.setOnClickListener(v->verify());box.addView(check);
        direct=new Button(this);direct.setText("备用：独立发送到配对 iPhone");direct.setEnabled(false);direct.setOnClickListener(v->start());box.addView(direct);
        status=new TextView(this);status.setTextSize(15);status.setText("准备就绪。仅本机实验版；已解除 64 KiB 限制，大文件仍需逐步验证。");status.setPadding(0,22,0,0);box.addView(status);
        if(checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS)!=PackageManager.PERMISSION_GRANTED)requestPermissions(new String[]{android.Manifest.permission.POST_NOTIFICATIONS},2);
        if(state!=null&&state.getString("selected")!=null)select(Uri.parse(state.getString("selected")));else receive(getIntent());if(getIntent().getBooleanExtra("check",false))verify();
    }
    public void onNewIntent(Intent i){super.onNewIntent(i);setIntent(i);receive(i);if(i.getBooleanExtra("check",false))verify();}
    private void receive(Intent i){if(Intent.ACTION_SEND.equals(i.getAction())){Uri u=i.getParcelableExtra(Intent.EXTRA_STREAM,Uri.class);select(u);}}
    protected void onActivityResult(int req,int result,Intent data){super.onActivityResult(req,result,data);if(req==1&&result==RESULT_OK&&data!=null)select(data.getData());}
    private void select(Uri uri){
        if(uri==null||!"content".equals(uri.getScheme())){status.setText("请选择可读取的本地文件。");return;}
        selected=uri;filename="Shared-file.bin";long size=-1;
        try(Cursor c=getContentResolver().query(uri,new String[]{OpenableColumns.DISPLAY_NAME,OpenableColumns.SIZE},null,null,null)){if(c!=null&&c.moveToFirst()){filename=c.getString(0);if(!c.isNull(1))size=c.getLong(1);}}
        catch(Exception e){selected=null;chosen.setText("文件未获得读取授权，请重新选择");setStatus("读取文件信息失败："+e.getMessage());send.setEnabled(false);return;}
        filename=AppFiles.name(filename);chosen.setText(filename+(size>=0?"\n"+android.text.format.Formatter.formatFileSize(this,size):""));send.setEnabled(!busy());
    }
    private void startQuickShare(boolean receive){
        if(busy()||checking||AppFiles.recoveryPending(this)||(!receive&&selected==null))return;
        Intent i=new Intent(this,QuickShareService.class).putExtra("receive",receive);
        if(!receive){i.setData(selected).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);i.setClipData(ClipData.newRawUri("file",selected));}
        startForegroundService(i);send.setEnabled(false);receiveButton.setEnabled(false);direct.setEnabled(false);
        setStatus("正在启动，请保持此页面，稍后将自动打开系统 Quick Share。");
    }
    private void start(){
        if(selected==null||busy()||checking||AppFiles.recoveryPending(this))return;
        Intent i=new Intent(this,ShareService.class).setData(selected).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION).putExtra("filename",filename);i.setClipData(ClipData.newRawUri("file",selected));
        startForegroundService(i);send.setEnabled(false);
    }
    private File priorJob(){
        String path=AppFiles.read(new File(getFilesDir(),"current-run.txt"),1024).trim(),prefix=new File(getFilesDir(),"jobs").getAbsolutePath()+"/";
        if(path.startsWith(prefix)&&path.substring(prefix.length()).matches("[A-Za-z0-9_-]+")&&new File(path).isDirectory())return new File(path);
        return null;
    }
    private void changeAutomatic(String mode){
        if(busy()||AppFiles.recoveryPending(this)){setStatus("请先等待当前会话恢复完成，再调整自动接入。");return;}
        if(checking)return;checking=true;setStatus("正在调整系统 Quick Share 自动接入…");
        new Thread(()->{String result;try{
            String cmd="id; [ \"$(id -u)\" = 0 ] || exit 77; echo ROOT_ACCESS_OK; exec /system/bin/sh /data/adb/modules/houji_qs_system79/auto/control.sh "+mode;
            java.lang.Process p=new ProcessBuilder("su","-c",cmd).redirectErrorStream(true).start();
            Thread timer=new Thread(()->{try{if(!p.waitFor(30,java.util.concurrent.TimeUnit.SECONDS))p.destroy();}catch(InterruptedException ignored){}});timer.setDaemon(true);timer.start();
            StringBuilder output=new StringBuilder();try(java.io.BufferedReader r=new java.io.BufferedReader(new java.io.InputStreamReader(p.getInputStream()))){String s;while((s=r.readLine())!=null)if(output.length()<5000)output.append(s).append('\n');}
            int rc=p.waitFor();timer.interrupt();
            if(rc==0&&mode.equals("enable")&&output.indexOf("AUTO_ENABLED=1")>=0&&output.indexOf("OBSERVER_RUNNING=1")>=0)result="自动接入已开启。以后直接使用系统 Quick Share 即可；进入分享页后请稍候，等待 iPhone 出现。";
            else if(rc==0&&mode.equals("disable")&&output.indexOf("AUTO_ENABLED=0")>=0)result="自动接入已暂停；仍可使用下方手动入口。";
            else result="自动接入尚未确认，请保留此提示。\n"+output;
        }catch(Exception e){result="自动接入调整失败："+e.getMessage();}
            checking=false;final String message=result;runOnUiThread(()->setStatus(message));
        },"quickshare-auto-control").start();
    }
    private void verify(){
        if(busy()){status.setText("会话进行中，请等待恢复完成。");return;}if(checking)return;checking=true;
        setStatus("正在检查 Root 与恢复状态…");
        new Thread(()->{String result;try{
            File job=priorJob();
            if(job==null){job=new File(getFilesDir(),"jobs/"+java.util.UUID.randomUUID());if(!job.mkdirs())throw new java.io.IOException("Cannot create check directory");}
            boolean quickshare=AppFiles.read(new File(job,"session-kind"),32).trim().equals("quickshare")||!new File(job,"device-changed.value").exists();
            File base=AppFiles.runtime(this,quickshare?"quickshare-runtime":"runtime");
            String mode=new File(job,"device-changed.value").exists()?"--verify-recovery":"--check-ready";
            String cmd="id; [ \"$(id -u)\" = 0 ] || exit 77; echo ROOT_ACCESS_OK; exec /system/bin/sh "+AppFiles.quote(new File(base,quickshare?"quickshare-runner.sh":"phone-runner.sh").getAbsolutePath())+" "+mode+" "+AppFiles.quote(base.getAbsolutePath())+" "+AppFiles.quote(job.getAbsolutePath());
            java.lang.Process p=new ProcessBuilder("su","-c",cmd).redirectErrorStream(true).start();
            Thread timeout=new Thread(()->{try{if(!p.waitFor(60,java.util.concurrent.TimeUnit.SECONDS))p.destroy();}catch(InterruptedException ignored){}},"root-check-timeout");timeout.setDaemon(true);timeout.start();
            StringBuilder out=new StringBuilder();try(java.io.BufferedReader r=new java.io.BufferedReader(new java.io.InputStreamReader(p.getInputStream()))){String l;while((l=r.readLine())!=null){if(out.length()<7000)out.append(l).append('\n');}}
            int code=p.waitFor();timeout.interrupt();boolean root=out.indexOf("ROOT_ACCESS_OK")>=0;
            if(code==0&&root){
                String recovery=quickshare?"Quick Share 启动准备检查通过。":"当前 C2 与原固件检查通过。";
                if(quickshare&&out.indexOf("MOBILE_RUNTIME_GRANTS_PENDING=true")>=0)recovery+="\n首次启动会补齐 Quick Share 所需权限。";
                if(mode.equals("--verify-recovery")){
                    org.json.JSONObject state=new org.json.JSONObject(AppFiles.read(new File(job,"status.json"),8192));
                    String phase=state.optString("phase");
                    if(!state.optBoolean("recovery_verified")||!(phase.equals("RECOVERED_C2_SAME_BOOT")||phase.equals("RECOVERED_C2")))throw new java.io.IOException("缺少已验证的恢复结果，请暂时不要发送");
                    recovery=phase.equals("RECOVERED_C2_SAME_BOOT")?"上次自动恢复检查通过（无需重启）。":"上次重启恢复检查通过。";
                }
                result="Root 已授权（uid=0）\n"+recovery;
            }
            else result=(root?"Root 已授权（uid=0），但设备检查未通过。\n暂时不要发送。\n":"应用未取得 Root，请检查 KernelSU 授权。\n")+"退出码："+code+"\n"+out;
        }catch(Exception e){result="检查失败："+e.getMessage();}
        checking=false;final String text=result;runOnUiThread(()->setStatus(text));},"root-check").start();
    }
    private void setStatus(String text){try{AppFiles.write(new File(getFilesDir(),"ui-status.txt"),text);}catch(Exception ignored){}status.setText(text);}
    protected void onResume(){super.onResume();handler.post(refresh);}
    protected void onPause(){handler.removeCallbacks(refresh);super.onPause();}
    protected void onSaveInstanceState(Bundle state){super.onSaveInstanceState(state);if(selected!=null)state.putString("selected",selected.toString());}
}
