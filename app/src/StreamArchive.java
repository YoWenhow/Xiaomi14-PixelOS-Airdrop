import java.io.*;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.Properties;
import java.util.zip.Deflater;

/** Constant-memory one-file ODC -> 128 KiB dvzip -> HTTP chunked writer. */
public final class StreamArchive {
    public static final long ODC_MAX=8589934591L;
    public static final long MAX_FILE=4294967296L;
    public static final int BLOCK=128*1024;
    public interface Progress {void update(long bytes,long total);}
    public static String hex(byte[] data){
        StringBuilder out=new StringBuilder(data.length*2);
        for(byte b:data)out.append(String.format(Locale.ROOT,"%02x",b&255));
        return out.toString();
    }
    public static byte[] nameBytes(String name)throws Exception{
        if(name==null||name.isEmpty()||name.equals(".")||name.equals("..")||name.indexOf('/')>=0||name.indexOf('\\')>=0)
            throw new IllegalArgumentException("Invalid source filename");
        for(int i=0;i<name.length();i++)if(Character.isISOControl(name.charAt(i)))throw new IllegalArgumentException("Control in filename");
        ByteBuffer encoded=StandardCharsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).encode(java.nio.CharBuffer.wrap(name));
        byte[] bytes=new byte[encoded.remaining()];encoded.get(bytes);
        if(bytes.length>1024)throw new IllegalArgumentException("Filename exceeds 1024 UTF-8 bytes");
        return bytes;
    }
    public static final class Source implements AutoCloseable{
        public final String name,sha;public final long size,mtime;
        private final FileInputStream input;
        private boolean closed;
        public Source(File directory)throws Exception{
            Properties p=new Properties();
            if(new File(directory,"transfer.properties").length()>65536)throw new IllegalArgumentException("Manifest exceeds 64 KiB");
            try(Reader r=new InputStreamReader(new FileInputStream(new File(directory,"transfer.properties")),StandardCharsets.UTF_8)){p.load(r);}
            name=p.getProperty("SourceFileName");nameBytes(name);
            size=Long.parseLong(p.getProperty("OriginalFileBytes"));
            mtime=Long.parseLong(p.getProperty("SourceMtime"));
            sha=p.getProperty("SourceSHA256");
            if(size<0||size>MAX_FILE||size>ODC_MAX)throw new IllegalArgumentException("Source exceeds 4 GiB practical cap");
            if(mtime<=0||mtime>ODC_MAX||mtime>System.currentTimeMillis()/1000+300)throw new IllegalArgumentException("Invalid source mtime");
            if(sha==null||!sha.matches("[0-9a-f]{64}"))throw new IllegalArgumentException("Invalid source SHA256");
            File file=new File(directory,"source.bin");
            if(!file.isFile())throw new IllegalArgumentException("Missing private source.bin snapshot");
            input=new FileInputStream(file);
            try{verify();}catch(Exception e){input.close();throw e;}
        }
        private void verify()throws Exception{
            if(input.getChannel().size()!=size)throw new IOException("Source size changed");
            MessageDigest digest=MessageDigest.getInstance("SHA-256");byte[] buf=new byte[32768];long count=0;int n;
            while((n=input.read(buf))!=-1){if(Thread.currentThread().isInterrupted())throw new InterruptedIOException("Source verification interrupted");count+=n;if(count>size)throw new IOException("Source grew");digest.update(buf,0,n);}
            if(count!=size||!hex(digest.digest()).equals(sha))throw new IOException("Source hash mismatch before connect");
            input.getChannel().position(0);
        }
        public void close()throws IOException{if(!closed){closed=true;input.close();}}
    }
    public static byte[] header(String path,long inode,long mode,long mtime,long bytes)throws Exception{
        byte[] name=path.getBytes(StandardCharsets.UTF_8);
        if(bytes<0||bytes>ODC_MAX||mtime<0||mtime>ODC_MAX)throw new IllegalArgumentException("ODC field overflow");
        long[] values={070707,0,inode,mode,0,0,1,0,mtime,name.length+1,bytes};
        int[] widths={6,6,6,6,6,6,6,6,11,6,11};
        ByteArrayOutputStream out=new ByteArrayOutputStream(80+name.length);
        for(int i=0;i<values.length;i++){
            String s=Long.toOctalString(values[i]);
            if(s.length()>widths[i])throw new IllegalArgumentException("ODC field overflow");
            for(int n=s.length();n<widths[i];n++)out.write('0');
            out.write(s.getBytes(StandardCharsets.US_ASCII));
        }
        if(out.size()!=76)throw new AssertionError("ODC header length");
        out.write(name);out.write(0);return out.toByteArray();
    }
    public static final class Chunked extends OutputStream{
        private final OutputStream out;private boolean ended;public long bytes;
        public Chunked(OutputStream out){this.out=out;}
        public void write(int b)throws IOException{write(new byte[]{(byte)b});}
        public void write(byte[] b,int off,int len)throws IOException{
            if(ended)throw new IOException("Chunked stream ended");
            if(len==0)return;
            out.write((Integer.toHexString(len)+"\r\n").getBytes(StandardCharsets.US_ASCII));
            out.write(b,off,len);out.write('\r');out.write('\n');bytes+=len;
        }
        public void finish()throws IOException{
            if(ended)throw new IOException("Chunked stream already ended");
            out.write("0\r\n\r\n".getBytes(StandardCharsets.US_ASCII));out.flush();ended=true;
        }
        public void flush()throws IOException{out.flush();}
    }
    public static final class Dvzip extends OutputStream{
        private final OutputStream out;
        private final byte[] raw=new byte[BLOCK],compressed=new byte[BLOCK+1024];
        private final Deflater deflater=new Deflater();private int used;public long bytes;
        public Dvzip(OutputStream out){this.out=out;}
        public void write(int b)throws IOException{raw[used++]=(byte)b;bytes++;if(used==raw.length)block(false);}
        public void write(byte[] b,int off,int len)throws IOException{
            while(len>0){int n=Math.min(len,raw.length-used);System.arraycopy(b,off,raw,used,n);used+=n;bytes+=n;off+=n;len-=n;if(used==raw.length)block(false);}
        }
        private void block(boolean empty)throws IOException{
            if(used==0&&!empty)return;
            deflater.setInput(raw,0,used);deflater.finish();int n=0;
            while(!deflater.finished()){
                int got=deflater.deflate(compressed,n,compressed.length-n);
                if(got<=0||n+got>compressed.length)throw new IOException("Compression made no progress");
                n+=got;
            }
            out.write(new byte[]{(byte)(n>>>24),(byte)(n>>>16),(byte)(n>>>8),(byte)n});out.write(compressed,0,n);
            deflater.reset();used=0;
        }
        public void finish()throws IOException{block(false);block(true);out.flush();}
        public void end(){deflater.end();}
    }
    /** Does not close socket. Writes terminal chunk only after size/hash verification. */
    public static long send(Source source,OutputStream wire,Progress progress)throws Exception{
        Chunked chunks=new Chunked(wire);Dvzip zip=new Dvzip(chunks);
        try{
            zip.write(header("./"+source.name,1,0100644,source.mtime,source.size));
            byte[] buf=new byte[32768];long count=0;
            MessageDigest digest=MessageDigest.getInstance("SHA-256");
            while(count<source.size){
                if(Thread.currentThread().isInterrupted())throw new InterruptedIOException("Upload interrupted");
                int n=source.input.read(buf,0,(int)Math.min(buf.length,source.size-count));
                if(n<0)throw new EOFException("Source shortened during upload");
                if(n==0)continue;
                zip.write(buf,0,n);digest.update(buf,0,n);count+=n;
                if(progress!=null)progress.update(count,source.size);
            }
            if(source.input.read()!=-1||source.input.getChannel().size()!=source.size||!hex(digest.digest()).equals(source.sha))
                throw new IOException("Source changed during upload; archive not finalized");
            zip.write(header("TRAILER!!!",0,0,0,0));
            int pad=(int)((512-zip.bytes%512)%512);
            zip.write(new byte[pad]);zip.finish();chunks.finish();
            return chunks.bytes;
        }finally{zip.end();}
    }
    public static void main(String[] args)throws Exception{
        if(args.length!=2)throw new IllegalArgumentException("HOST TEST: PAYLOAD_DIR OUTPUT_CHUNKED_FILE");
        try(Source source=new Source(new File(args[0]));OutputStream output=new FileOutputStream(args[1])){
            long bytes=send(source,output,null);System.out.println("source_bytes="+source.size+" dvzip_bytes="+bytes);
        }
    }
}
