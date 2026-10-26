package org.houji.airdrop;

import android.content.Context;
import android.content.res.AssetManager;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Properties;
import java.util.UUID;

final class AppFiles {
    static final long MAX_SOURCE=4L*1024*1024*1024;
    static File runtime(Context c)throws Exception {
        return runtime(c,"runtime");
    }
    static File runtime(Context c,String folder)throws Exception {
        if(!folder.equals("runtime")&&!folder.equals("quickshare-runtime"))throw new IOException("Invalid runtime");
        File base=new File(c.getFilesDir(),folder);
        String manifest;
        try(InputStream in=c.getAssets().open(folder+"/SHA256SUMS");ByteArrayOutputStream out=new ByteArrayOutputStream()) {
            byte[] b=new byte[4096];int n;while((n=in.read(b))!=-1)out.write(b,0,n);
            manifest=new String(out.toByteArray(),StandardCharsets.UTF_8);
            if(manifest.equals(read(new File(base,"SHA256SUMS"),16384)))return base;
        }
        if(!base.isDirectory()&&!base.mkdirs())throw new IOException("Cannot create runtime directory");
        File sums=new File(base,"SHA256SUMS");if(sums.exists()&&!sums.delete())throw new IOException("Cannot replace runtime manifest");
        for(String child:c.getAssets().list(folder))if(!child.equals("SHA256SUMS"))copyAssets(c.getAssets(),folder+"/"+child,new File(base,child));
        write(sums,manifest); // Publish completion only after every asset was copied.
        return base;
    }
    static void copyAssets(AssetManager assets,String path,File dst)throws Exception {
        String[] children=assets.list(path);
        if(children!=null && children.length>0) {
            if(!dst.isDirectory()&&!dst.mkdirs())throw new IOException("Cannot create runtime directory");
            for(String name:children)copyAssets(assets,path+"/"+name,new File(dst,name));
        } else {
            try(InputStream in=assets.open(path);OutputStream out=new FileOutputStream(dst)) {
                byte[] b=new byte[65536];int n;while((n=in.read(b))!=-1)out.write(b,0,n);
            }
        }
    }
    static void write(File f,String text)throws IOException {
        try(Writer w=new OutputStreamWriter(new FileOutputStream(f),StandardCharsets.UTF_8)){w.write(text);}
    }
    static String read(File f,int limit) {
        try(InputStream in=new FileInputStream(f);ByteArrayOutputStream b=new ByteArrayOutputStream()) {
            byte[] buf=new byte[2048];int n;while((n=in.read(buf))!=-1 && b.size()<limit)b.write(buf,0,Math.min(n,limit-b.size()));
            return new String(b.toByteArray(),StandardCharsets.UTF_8);
        }catch(Exception e){return "";}
    }
    static String quote(String s){return "'"+s.replace("'","'\\''")+"'";}
    static boolean recoveryPending(Context c){
        String path=read(new File(c.getFilesDir(),"current-run.txt"),1024).trim(),prefix=new File(c.getFilesDir(),"jobs").getAbsolutePath()+"/";
        if(!path.startsWith(prefix)||!path.substring(prefix.length()).matches("[A-Za-z0-9_-]+"))return false;
        File job=new File(path);if(!new File(job,"device-changed.value").exists())return false;
        try{org.json.JSONObject state=new org.json.JSONObject(read(new File(job,"status.json"),8192));String phase=state.optString("phase");
            return !state.optBoolean("recovery_verified")||!(phase.equals("RECOVERED_C2_SAME_BOOT")||phase.equals("RECOVERED_C2"));
        }catch(Exception e){return true;}
    }
    static String name(String original) {
        String s=original==null?"Shared-file.bin":original;
        s=s.replaceAll("[\\\\/\\p{Cntrl}]","_").trim();
        if(s.isEmpty()||s.equals(".")||s.equals(".."))s="Shared-file.bin";
        while(s.getBytes(StandardCharsets.UTF_8).length>240)s=s.substring(0,s.offsetByCodePoints(s.length(),-1));
        return s;
    }
    static String xml(String s){return s.replace("&","&amp;").replace("<","&lt;").replace(">","&gt;").replace("\"","&quot;").replace("'","&apos;");}
    static String uti(String filename,String mime) {
        String n=filename.toLowerCase(java.util.Locale.ROOT);
        if(n.endsWith(".docx"))return "org.openxmlformats.wordprocessingml.document";
        if(n.endsWith(".pdf"))return "com.adobe.pdf";
        if(n.endsWith(".jpg")||n.endsWith(".jpeg"))return "public.jpeg";
        if(n.endsWith(".png"))return "public.png";
        if(n.endsWith(".txt"))return "public.plain-text";
        if(n.endsWith(".zip"))return "public.zip-archive";
        return "public.data";
    }
    static void payload(File job,String filename,String mime,long bytes,String digest)throws Exception {
        String id=UUID.randomUUID().toString().toUpperCase(java.util.Locale.ROOT);
        String ask="<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict>"+
            "<key>SenderComputerName</key><string>Xiaomi 14 PixelOS Test</string>"+
            "<key>BundleID</key><string>com.apple.finder</string><key>SenderModelName</key><string>Xiaomi 14</string>"+
            "<key>SenderID</key><string>872f5c2f0995</string><key>ConvertMediaFormats</key><false/>"+
            "<key>TransferID</key><dict><key>id</key><string>"+id+"</string></dict>"+
            "<key>TransferType</key><dict><key>files</key><dict/></dict><key>Files</key><array><dict>"+
            "<key>FileName</key><string>"+xml(filename)+"</string><key>FileType</key><string>"+uti(filename,mime)+"</string>"+
            "<key>FileBomPath</key><string>./"+xml(filename)+"</string><key>FileIsDirectory</key><false/>"+
            "<key>ConvertMediaFormats</key><integer>0</integer><key>FileSize</key><integer>"+bytes+"</integer>"+
            "<key>ShouldConvertMediaFormats</key><false/></dict></array></dict></plist>";
        byte[] a=ask.getBytes(StandardCharsets.UTF_8);write(new File(job,"ask.plist"),ask);
        Properties props=new Properties();props.setProperty("TransferID",id);props.setProperty("AskSHA256",hex(MessageDigest.getInstance("SHA-256").digest(a)));
        props.setProperty("SourceFileName",filename);props.setProperty("OriginalFileBytes",Long.toString(bytes));
        props.setProperty("SourceMtime",Long.toString(System.currentTimeMillis()/1000));props.setProperty("SourceSHA256",digest);
        try(Writer w=new OutputStreamWriter(new FileOutputStream(new File(job,"transfer.properties")),StandardCharsets.UTF_8)){props.store(w,"Private one-file transfer");}
    }
    static String hex(byte[] data){StringBuilder s=new StringBuilder();for(byte v:data)s.append(String.format(java.util.Locale.ROOT,"%02x",v&255));return s.toString();}
    static void copy(File a,File b)throws Exception {try(InputStream in=new FileInputStream(a);OutputStream out=new FileOutputStream(b)){byte[] buf=new byte[65536];int n;while((n=in.read(buf))!=-1)out.write(buf,0,n);}}
}
