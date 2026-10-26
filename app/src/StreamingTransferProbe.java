import android.os.ParcelFileDescriptor;
import android.system.Os;
import android.system.OsConstants;
import java.io.ByteArrayOutputStream;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.InvocationTargetException;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.NetworkInterface;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.security.cert.X509Certificate;
import java.util.Arrays;
import java.util.Enumeration;
import java.util.Locale;
import javax.net.ssl.KeyManagerFactory;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLSession;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509TrustManager;

/** Streaming one-file transfer; fixed known-peer TLS pin and bounded HTTP responses. */
public final class StreamingTransferProbe {
    static final String EMPTY_PLIST_HEX = "62706c6973743030d0080000000000000101000000000000000100000000000000000000000000000009";
    static final int LIMIT = 65536;
    static volatile Socket active;
    static volatile boolean finished;

    public static void main(String[] args) {
        if(args.length==1 && args[0].equals("--selftest")) {
            try {selftest();return;} catch(Exception e){throw new RuntimeException(e);}
        }
        try {run(args);} catch(Throwable error){
            System.err.println("transfer_failed="+error.getClass().getSimpleName()+": "+error.getMessage());
            System.exit(1);
        }
    }
    public static long deadlineMillis(long bytes) {
        return Math.min(900000L,180000L+Math.max(0,(bytes+65535L)/65536L)*1000L);
    }
    /** Safe for embedding: never exits the process; caller retains its finally cleanup. */
    public static synchronized void run(final String[] args) throws Exception {
        if(args.length!=7)throw new IllegalArgumentException("IFACE IFINDEX SOURCE_HEX32 PEER_HEX32 PORT P12_PATH PAYLOAD_DIR");
        long budget=deadlineMillis(new java.io.File(args[6],"source.bin").length());
        java.io.File propertiesFile=new java.io.File(args[6],"transfer.properties");
        if(propertiesFile.length()>65536)throw new IllegalArgumentException("Manifest exceeds 64 KiB");
        java.util.Properties options=new java.util.Properties();
        try(java.io.Reader reader=new java.io.InputStreamReader(new FileInputStream(propertiesFile),StandardCharsets.UTF_8)){options.load(reader);}
        String override=options.getProperty("TransferTimeoutSeconds");
        if(override!=null){long seconds=Long.parseLong(override);if(seconds<120||seconds>900)throw new IllegalArgumentException("Timeout must be 120..900 seconds");budget=seconds*1000;}
        final long deadline=budget;
        final java.io.File cancel=new java.io.File(args[6],"cancel");
        if(cancel.exists())throw new java.io.InterruptedIOException("Transfer cancelled before verification");
        final Thread owner=Thread.currentThread();
        final java.util.concurrent.atomic.AtomicBoolean done=new java.util.concurrent.atomic.AtomicBoolean();
        final java.util.concurrent.atomic.AtomicBoolean expired=new java.util.concurrent.atomic.AtomicBoolean();
        active=null;
        Thread watchdog=new Thread(new Runnable(){public void run(){
            long end=System.nanoTime()+deadline*1000000L;
            while(!done.get()){
                boolean cancelled=cancel.exists();
                if(cancelled||System.nanoTime()>=end){
                    expired.set(true);System.err.println(cancelled?"transfer_cancelled=true":"transfer_deadline_exceeded_ms="+deadline);
                    Socket timedSocket=active;
                    owner.interrupt();
                    try{if(timedSocket!=null)timedSocket.close();}catch(Exception ignored){}
                    return;
                }
                try{Thread.sleep(500);}catch(InterruptedException e){return;}
            }
        }},"stream-transfer-deadline");
        watchdog.setDaemon(true);watchdog.start();
        System.out.println("transfer_deadline_ms="+deadline+" source_cap_bytes="+StreamArchive.MAX_FILE);
        try{execute(args);if(expired.get())throw new java.io.InterruptedIOException("Transfer deadline exceeded");}
        finally{
            done.set(true);watchdog.interrupt();
            try{if(active!=null)active.close();}catch(Exception ignored){}
            if(expired.get())Thread.interrupted();
            System.out.flush();System.err.flush();
        }
    }

    private static void execute(String[] args) throws Exception {
        if (args.length != 7) throw new IllegalArgumentException("IFACE IFINDEX SOURCE_HEX32 PEER_HEX32 PORT P12_PATH PAYLOAD_DIR");
        try(StreamArchive.Source sourceFile=new StreamArchive.Source(new java.io.File(args[6]))) {
        String iface = args[0];
        if (!iface.matches("[A-Za-z0-9_.-]{1,15}")) throw new IllegalArgumentException("invalid interface name");
        int ifindex = Integer.parseInt(args[1]);
        int port = Integer.parseInt(args[4]);
        if (ifindex <= 0 || port < 1 || port > 65535) throw new IllegalArgumentException("invalid index or port");
        byte[] sourceBytes = addressBytes(args[2]);
        byte[] peerBytes = addressBytes(args[3]);
        NetworkInterface ni = NetworkInterface.getByName(iface);
        if (ni == null || ni.getIndex() != ifindex || !ni.isUp()) throw new IllegalStateException("interface/index mismatch or down");
        boolean sourcePresent = false;
        for (Enumeration<InetAddress> addresses = ni.getInetAddresses(); addresses.hasMoreElements(); )
            if (Arrays.equals(addresses.nextElement().getAddress(), sourceBytes)) sourcePresent = true;
        if (!sourcePresent) throw new IllegalStateException("source is not assigned to interface");
        Inet6Address source = Inet6Address.getByAddress(null, sourceBytes, ifindex);
        Inet6Address peer = Inet6Address.getByAddress(null, peerBytes, ifindex);
        if (!source.isLinkLocalAddress() || !peer.isLinkLocalAddress() || Arrays.equals(sourceBytes, peerBytes))
            throw new IllegalArgumentException("distinct link-local IPv6 addresses required");
        System.out.println("interface=" + iface + " ifindex=" + ifindex + " source=" + source.getHostAddress() + " peer=" + peer.getHostAddress() + " port=" + port);

        KeyStore identity = KeyStore.getInstance("PKCS12");
        char[] password = "temporary-test".toCharArray();
        try (FileInputStream in = new FileInputStream(args[5])) { identity.load(in, password); }
        KeyManagerFactory km = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
        km.init(identity, password);
        Arrays.fill(password, '\0');
        SSLContext context = SSLContext.getInstance("TLS");
        context.init(km.getKeyManagers(), new TrustManager[] { new X509TrustManager() {
            public X509Certificate[] getAcceptedIssuers() { return new X509Certificate[0]; }
            public void checkClientTrusted(X509Certificate[] chain, String authType) { throw new UnsupportedOperationException("client only"); }
            public void checkServerTrusted(X509Certificate[] chain, String authType) throws java.security.cert.CertificateException {
                if (chain == null || chain.length == 0) throw new java.security.cert.CertificateException("empty peer chain");
                // Only this SSLContext, this connected socket, and this already verified peer.
            }
        } }, new SecureRandom());

        Socket raw = null;
        for(int attempt=1;attempt<=3;attempt++) {
            raw=new Socket();active=raw;
            raw.bind(new InetSocketAddress(source,0));
            try(ParcelFileDescriptor duplicate=ParcelFileDescriptor.fromSocket(raw)) {
                Os.class.getMethod("setsockoptIfreq",FileDescriptor.class,int.class,int.class,String.class)
                    .invoke(null,duplicate.getFileDescriptor(),OsConstants.SOL_SOCKET,OsConstants.SO_BINDTODEVICE,iface);
            }
            System.out.println("tcp_connect_attempt="+attempt+" bind_source_ok=true bind_device_ok=true");System.out.flush();
            try {raw.connect(new InetSocketAddress(peer,port),12000);break;}
            catch(SocketTimeoutException timeout) {
                raw.close();System.out.println("tcp_connect_timeout_attempt="+attempt);System.out.flush();
                if(attempt==3)throw timeout;
                Thread.sleep(250);
            }
        }
        System.out.println("tcp_connected=true");
        SSLSocket tls = (SSLSocket) context.getSocketFactory().createSocket(raw, peer.getHostAddress(), port, true);
        active = tls;
        tls.setUseClientMode(true);
        tls.setEnabledProtocols(new String[] { "TLSv1.2", "TLSv1.3" });
        tls.setSoTimeout(12000);
        tls.startHandshake();
        SSLSession session = tls.getSession();
        System.out.println("tls_connected=true protocol=" + session.getProtocol() + " cipher=" + session.getCipherSuite());
        System.out.println("peer_certificate_sha256=" + hex(MessageDigest.getInstance("SHA-256").digest(session.getPeerCertificates()[0].getEncoded())));
        System.out.println("client_certificate_sent=" + (session.getLocalCertificates() != null));

        String cert = hex(MessageDigest.getInstance("SHA-256").digest(session.getPeerCertificates()[0].getEncoded()));
        if (!cert.equals("a2fcc355484320bfea58a51c56aa013bd4ab5adf35c8dc047cf53e812f065e2d"))
            throw new SecurityException("Known iPhone certificate changed; no Ask or Upload sent");
        System.out.println("known_peer_certificate_matched=true");
        String dir=args[6];
        java.util.Properties props=new java.util.Properties();
        try(java.io.Reader reader=new java.io.InputStreamReader(new FileInputStream(dir+"/transfer.properties"),StandardCharsets.UTF_8)){props.load(reader);}
        String transferId=props.getProperty("TransferID");
        if(transferId==null || !transferId.matches("[A-F0-9]{8}(-[A-F0-9]{4}){3}-[A-F0-9]{12}"))
            throw new IllegalArgumentException("invalid TransferID");
        byte[] ask=file(dir+"/ask.plist");
        if(!hex(MessageDigest.getInstance("SHA-256").digest(ask)).equals(props.getProperty("AskSHA256")))
            throw new IllegalStateException("Ask hash mismatch");
        System.out.println("source_total_bytes="+sourceFile.size+" source_sha_verified=true streaming=true");
        String host="["+Inet6Address.getByAddress(null,peerBytes,-1).getHostAddress()+"]:"+port;
        InputStream in=new java.io.BufferedInputStream(tls.getInputStream());
        OutputStream out=tls.getOutputStream();
        System.out.println("ask_first_on_fresh_pinned_connection=true");
        tls.setSoTimeout(45000);
        System.out.println("ask_waiting_for_user=true transfer_id="+transferId);System.out.flush();
        request(out,host,"Ask","application/octet-stream",ask,transferId,false,0);
        Response accepted=response(in,"ask");
        requireOk(accepted,"Ask (upload requires acceptance)");
        if("close".equalsIgnoreCase(accepted.headers.get("connection"))) throw new IllegalStateException("Ask closed persistent connection");
        System.out.println("ask_accepted_http=true upload_starting=true");System.out.flush();
        tls.setSoTimeout(90000);
        streamUpload(out,transferId,sourceFile);
        Response uploaded=response(in,"upload");
        requireOk(uploaded,"Upload");
        System.out.println("upload_http_success=true actual_file_receipt_requires_user_verification=true");
        }
    }

    static byte[] file(String path)throws Exception{
        try(InputStream in=new FileInputStream(path)){
            ByteArrayOutputStream b=new ByteArrayOutputStream();byte[] buf=new byte[4096];int n;
            while((n=in.read(buf))!=-1){if(b.size()+n>65536)throw new IllegalArgumentException("Ask metadata exceeds 64 KiB");b.write(buf,0,n);}
            return b.toByteArray();
        }
    }
    static void request(OutputStream out,String host,String path,String type,byte[] body,String id,boolean chunked,long originalBytes)throws Exception{
        String head="POST /"+path+" HTTP/1.1\r\nHost: "+host+"\r\nContent-Type: "+type+
            "\r\nUser-Agent: AirDrop/1.0\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n";
        if(id!=null && path.equals("Upload"))head+="TransferID: "+id+"\r\n";
        head+=chunked?"TotalBytes: "+originalBytes+"\r\nTransfer-Encoding: chunked\r\n":"Content-Length: "+body.length+"\r\n";
        if(chunked && path.equals("Upload")) {
            byte[] pseudo=new byte[16],push=new byte[32];SecureRandom random=new SecureRandom();random.nextBytes(pseudo);random.nextBytes(push);
            String pseudonym="pseud:"+java.util.Base64.getUrlEncoder().withoutPadding().encodeToString(pseudo);
            String pushToken=hex(push).toUpperCase(Locale.ROOT);
            // Ephemeral anonymous protocol placeholders, not Apple account credentials.
            head="POST /Upload HTTP/1.1\r\nUser-Agent: AirDrop/1.0\r\nTotalBytes: "+originalBytes+
                "\r\nContent-Type: application/x-dvzip\r\nSenderPseudonym: "+pseudonym+
                "\r\nSenderPushToken: "+pushToken+"\r\nTransferID: "+id+
                "\r\nConnection: keep-alive\r\nTransfer-Encoding: chunked\r\n";
            System.out.println("upload_header_profile=reference_rawhdr_google_source_total ephemeral_identifiers=true");
        }
        out.write((head+"\r\n").getBytes(StandardCharsets.US_ASCII));
        if(chunked)out.write((Integer.toHexString(body.length)+"\r\n").getBytes(StandardCharsets.US_ASCII));
        out.write(body);
        if(chunked)out.write("\r\n0\r\n\r\n".getBytes(StandardCharsets.US_ASCII));
        out.flush();System.out.println(path.toLowerCase(Locale.ROOT)+"_sent=true body_bytes="+body.length);System.out.flush();
    }
    static void streamUpload(OutputStream out,String id,StreamArchive.Source file)throws Exception {
        byte[] pseudo=new byte[16],push=new byte[32];SecureRandom random=new SecureRandom();
        random.nextBytes(pseudo);random.nextBytes(push);
        String head="POST /Upload HTTP/1.1\r\nUser-Agent: AirDrop/1.0\r\nTotalBytes: "+file.size+
            "\r\nContent-Type: application/x-dvzip\r\nSenderPseudonym: pseud:"+
            java.util.Base64.getUrlEncoder().withoutPadding().encodeToString(pseudo)+
            "\r\nSenderPushToken: "+hex(push).toUpperCase(Locale.ROOT)+"\r\nTransferID: "+id+
            "\r\nConnection: keep-alive\r\nTransfer-Encoding: chunked\r\n\r\n";
        out.write(head.getBytes(StandardCharsets.US_ASCII));
        long wire=StreamArchive.send(file,out,new StreamArchive.Progress(){
            long lastBytes,lastTime=System.nanoTime();
            public void update(long bytes,long total){
                long now=System.nanoTime();
                if(bytes==total||bytes-lastBytes>=1024*1024||now-lastTime>=1000000000L){
                    System.out.println("upload_progress_bytes="+bytes+" total_bytes="+total);
                    System.out.flush();lastBytes=bytes;lastTime=now;
                }
            }
        });
        System.out.println("upload_sent=true source_bytes="+file.size+" dvzip_bytes="+wire+" source_sha_verified_after_read=true");
        System.out.flush();
    }
    static final class Response{
        int status;byte[] body;
        java.util.Map<String,String> headers=new java.util.LinkedHashMap<String,String>();
    }
    static Response response(InputStream in,String phase)throws Exception{
        ByteArrayOutputStream raw=new ByteArrayOutputStream();
        try{
            Response r=parse(in,raw);
            System.out.println(phase+"_http_status="+r.status+" body_bytes="+r.body.length+" complete=true");
            System.out.flush();
            return r;
        }catch(Exception e){
            System.out.println(phase+"_complete=false partial_response_bytes="+raw.size());System.out.flush();throw e;
        }
    }
    static Response parse(InputStream in,ByteArrayOutputStream raw)throws Exception{
        Response r=new Response();String first=line(in,raw);
        if(!first.matches("HTTP/1\\.[01] [0-9]{3}( .*)?"))throw new java.io.IOException("invalid HTTP status");
        r.status=Integer.parseInt(first.substring(9,12));
        String s;
        while(!(s=line(in,raw)).isEmpty()){
            if(raw.size()>16384)throw new java.io.IOException("header limit");
            int colon=s.indexOf(':');if(colon<1)throw new java.io.IOException("bad header");
            String key=s.substring(0,colon).toLowerCase(Locale.ROOT),value=s.substring(colon+1).trim();
            if(r.headers.put(key,value)!=null)throw new java.io.IOException("duplicate header "+key);
        }
        if(!"identity".equalsIgnoreCase(r.headers.getOrDefault("content-encoding","identity")))throw new java.io.IOException("unsupported content encoding");
        ByteArrayOutputStream body=new ByteArrayOutputStream();
        String te=r.headers.get("transfer-encoding"),cl=r.headers.get("content-length");
        if(te!=null){
            if(!te.equalsIgnoreCase("chunked") || cl!=null)throw new java.io.IOException("ambiguous HTTP framing");
            while(true){
                String size=line(in,raw).split(";",2)[0].trim();
                if(!size.matches("[0-9a-fA-F]{1,8}"))throw new java.io.IOException("bad chunk size");
                long n=Long.parseLong(size,16);
                if(n==0){while(!line(in,raw).isEmpty()){}break;}
                exact(in,raw,body,n);
                if(!line(in,raw).isEmpty())throw new java.io.IOException("bad chunk delimiter");
            }
        }else if(cl!=null){
            if(!cl.matches("[0-9]+"))throw new java.io.IOException("invalid content length");
            exact(in,raw,body,Long.parseLong(cl));
        }else if(r.status!=204 && r.status!=304){throw new java.io.IOException("unframed persistent HTTP response");}
        r.body=body.toByteArray();return r;
    }
    static String line(InputStream in,ByteArrayOutputStream raw)throws Exception{
        ByteArrayOutputStream b=new ByteArrayOutputStream();
        for(int i=0;i<16384;i++){
            int c=one(in,raw);
            if(c=='\r'){if(one(in,raw)!='\n')throw new java.io.IOException("expected LF");return new String(b.toByteArray(),StandardCharsets.ISO_8859_1);}
            if(c=='\n')throw new java.io.IOException("bare LF");b.write(c);
        }throw new java.io.IOException("line limit");
    }
    static int one(InputStream in,ByteArrayOutputStream raw)throws Exception{
        if(raw.size()>=LIMIT)throw new java.io.IOException("HTTP limit");
        int c=in.read();if(c<0)throw new java.io.EOFException("truncated HTTP response");raw.write(c);return c;
    }
    static void exact(InputStream in,ByteArrayOutputStream raw,ByteArrayOutputStream body,long n)throws Exception{
        if(n<0 || n>LIMIT-raw.size())throw new java.io.IOException("body limit");
        for(long i=0;i<n;i++)body.write(one(in,raw));
    }
    static void requireOk(Response r,String phase){if(r.status!=200)throw new IllegalStateException(phase+" HTTP "+r.status+"; no subsequent request");}
    static byte[] addressBytes(String value){if(!value.matches("[0-9A-Fa-f]{32}"))throw new IllegalArgumentException("32 IPv6 hex digits required");return unhex(value);}
    static byte[] unhex(String value){byte[] b=new byte[value.length()/2];for(int i=0;i<b.length;i++)b[i]=(byte)Integer.parseInt(value.substring(i*2,i*2+2),16);return b;}
    static String hex(byte[] data){StringBuilder b=new StringBuilder();for(byte v:data)b.append(String.format(Locale.ROOT,"%02x",v&255));return b.toString();}
    static void selftest()throws Exception{
        byte[] combined=("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nxy").getBytes(StandardCharsets.US_ASCII);
        InputStream in=new java.io.ByteArrayInputStream(combined);
        if(!Arrays.equals(parse(in,new ByteArrayOutputStream()).body,"abc".getBytes(StandardCharsets.US_ASCII)))throw new AssertionError();
        if(!Arrays.equals(parse(in,new ByteArrayOutputStream()).body,"xy".getBytes(StandardCharsets.US_ASCII)) || in.read()!=-1)throw new AssertionError("persistent stream boundary");
        String[] invalid={"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nx","HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n","HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n100000\r\n"};
        for(String s:invalid){boolean rejected=false;try{parse(new java.io.ByteArrayInputStream(s.getBytes(StandardCharsets.US_ASCII)),new ByteArrayOutputStream());}catch(java.io.IOException e){rejected=true;}if(!rejected)throw new AssertionError("invalid HTTP accepted");}
        ByteArrayOutputStream sent=new ByteArrayOutputStream();request(sent,"[fe80::1]:8770","Upload","application/x-dvzip",new byte[]{1,2,3},"00000000-0000-0000-0000-000000000001",true,89);
        String wire=new String(sent.toByteArray(),StandardCharsets.ISO_8859_1);
        if(!wire.contains("TotalBytes: 89\r\n") || !wire.endsWith("3\r\n\u0001\u0002\u0003\r\n0\r\n\r\n"))throw new AssertionError("chunked upload framing");
        if(wire.contains("\r\nHost:") || wire.contains("\r\nAccept"))throw new AssertionError("unexpected upload header");
        if(!wire.matches("(?s).*\r\nSenderPseudonym: pseud:[A-Za-z0-9_-]{22}\r\nSenderPushToken: [A-F0-9]{64}\r\nTransferID: .*"))throw new AssertionError("anonymous header format");
        System.out.println("persistent HTTP boundaries, framing rejection, chunked request, distinct source total and reference headers passed");
    }
}
