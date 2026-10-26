import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Root app_process helper. The caller owns the private Mosey namespace and reboot. */
public final class MobileSession {
    static final String DESCRIPTOR = "com.google.android.moseyservice.IMoseyService";
    static final String IFACE = "mosey0", PEER = "fe80::f4ad:41ff:fe6b:7230";
    static final String PEER_HEX = "fe80000000000000f4ad41fffe6b7230";
    static final String PEER_MAC = "f6:ad:41:6b:72:30", TABLE = "10073", PRIORITY = "10999";
    static final String IP = "/system/bin/ip";
    static final long SESSION_MS = 900000, DISCOVERY_MS = 75000;
    final Binder lifetime = new Binder();
    final File base, run, cancel;
    final long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(SESSION_MS);
    volatile Process activeProcess;
    volatile boolean activeProcessCancellable;
    volatile boolean finished;
    IBinder service;
    boolean startAttempted, routeAdded, ruleAdded;
    String sourceHex, sourceText;
    int interfaceIndex;

    MobileSession(String basePath, String runPath) throws IOException {
        if (!new File(basePath).isAbsolute() || !new File(runPath).isAbsolute())
            throw new IOException("BASE and RUN must be absolute");
        base = new File(basePath).getCanonicalFile();
        run = new File(runPath).getCanonicalFile();
        if (!base.isDirectory() || !run.isDirectory()) throw new IOException("BASE or RUN absent");
        cancel = new File(run, "cancel");
    }

    static void log(String value) { System.out.println(value); System.out.flush(); }
    void check() throws IOException, InterruptedException {
        if (cancel.exists()) throw new IOException("session cancelled");
        if (System.nanoTime() >= deadline) throw new IOException("session deadline exceeded");
        if (Thread.currentThread().isInterrupted()) throw new InterruptedException("session interrupted");
    }
    void pause(long millis) throws Exception { check(); Thread.sleep(millis); check(); }
    Thread watchdog(final Thread owner) {
        Thread t = new Thread(() -> {
            try {
                while (!finished && System.nanoTime() < deadline) Thread.sleep(250);
                if (!finished) {
                    log("step=deadline");
                    try { cancel.createNewFile(); } catch (IOException e) { log("cancel_write_error=" + e); }
                    Process p = activeProcess;
                    if (p != null && activeProcessCancellable) p.destroy();
                    owner.interrupt();
                }
            } catch (InterruptedException ignored) { }
        }, "mobile-session-deadline");
        t.setDaemon(true); t.start(); return t;
    }

    static final class Result {
        final int code; final String text;
        Result(int code, String text) { this.code = code; this.text = text; }
    }
    Result command(boolean cleanup, long timeoutMs, String... argv) throws Exception {
        if (!cleanup) check();
        Process process = new ProcessBuilder(argv).redirectErrorStream(true).start();
        activeProcessCancellable = !cleanup;
        activeProcess = process;
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        AtomicReference<IOException> readError = new AtomicReference<>();
        Thread drain = new Thread(() -> {
            try (InputStream stream = process.getInputStream()) {
                byte[] buffer = new byte[4096]; int n;
                while ((n = stream.read(buffer)) != -1) {
                    if (output.size() + n > 1048576) throw new IOException("command output exceeds 1 MiB");
                    output.write(buffer, 0, n);
                }
            } catch (IOException e) { readError.set(e); process.destroy(); }
        }, "mobile-command-output");
        drain.setDaemon(true); drain.start();
        try {
            long end = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(timeoutMs);
            while (true) {
                try { if (process.waitFor(100, TimeUnit.MILLISECONDS)) break; }
                catch (InterruptedException e) { if (!cleanup) throw e; }
                if (!cleanup) check();
                if (System.nanoTime() >= end) throw new IOException("command timed out: " + argv[0]);
            }
            long drainEnd = System.nanoTime() + TimeUnit.SECONDS.toNanos(1);
            while (drain.isAlive() && System.nanoTime() < drainEnd) {
                try { drain.join(100); } catch (InterruptedException e) { if (!cleanup) throw e; }
            }
            if (drain.isAlive()) throw new IOException("command output did not close");
            if (readError.get() != null) throw readError.get();
            return new Result(process.exitValue(), new String(output.toByteArray(), StandardCharsets.UTF_8));
        } finally {
            process.destroy();
            if (activeProcess == process) activeProcess = null;
        }
    }
    Result required(boolean cleanup, String... argv) throws Exception {
        Result r = command(cleanup, 4000, argv);
        if (r.code != 0) throw new IOException("command exit=" + r.code + " " + Arrays.toString(argv) + ": " + r.text.trim());
        return r;
    }

    Parcel request() { Parcel p = Parcel.obtain(); p.writeInterfaceToken(DESCRIPTOR); return p; }
    Parcel transact(int code, Parcel input) throws Exception {
        Parcel reply = Parcel.obtain();
        try {
            if (!service.transact(code, input, reply, 0)) throw new IOException("unhandled Mosey transaction " + code);
            reply.readException(); return reply;
        } catch (Exception e) { reply.recycle(); throw e; }
        finally { input.recycle(); }
    }
    void start() throws Exception {
        log("step=mosey_start");
        Class<?> sm = Class.forName("android.os.ServiceManager");
        long waitEnd = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
        do {
            check();
            service = (IBinder) sm.getDeclaredMethod("checkService", String.class).invoke(null, DESCRIPTOR + "/default");
            if (service != null) break;
            pause(200);
        } while (System.nanoTime() < waitEnd);
        if (service == null || !DESCRIPTOR.equals(service.getInterfaceDescriptor()))
            throw new IOException("Mosey service absent or wrong descriptor");
        Parcel reply = transact(16777215, request());
        try { log("mosey_interface_version=" + reply.readInt()); } finally { reply.recycle(); }
        Parcel update = request(); update.writeInt(1); int begin = update.dataPosition(); update.writeInt(0);
        update.writeString("CN"); update.writeByteArray(null); int end = update.dataPosition();
        update.setDataPosition(begin); update.writeInt(end - begin); update.setDataPosition(end);
        reply = transact(3, update); reply.recycle();
        check();
        Parcel input = request(); input.writeInt(1); begin = input.dataPosition(); input.writeInt(0);
        input.writeIntArray(new int[]{6}); input.writeStrongBinder(lifetime);
        input.writeInt(Integer.MAX_VALUE); input.writeByteArray(null); end = input.dataPosition();
        input.setDataPosition(begin); input.writeInt(end - begin); input.setDataPosition(end);
        startAttempted = true;
        reply = transact(1, input); reply.recycle();
    }
    void stop() throws Exception {
        if (!startAttempted) return;
        log("step=mosey_stop");
        Parcel p = request(); p.writeInt(1); p.writeInt(4);
        Parcel reply = transact(2, p); reply.recycle();
        log("MOBILE_STOP_OK");
    }

    void obtainInterface() throws Exception {
        long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(12);
        do {
            check();
            NetworkInterface ni = NetworkInterface.getByName(IFACE);
            if (ni != null && ni.getIndex() > 0 && ni.getIndex() < 65536) {
                Enumeration<InetAddress> addresses = ni.getInetAddresses();
                while (addresses.hasMoreElements()) {
                    InetAddress address = addresses.nextElement();
                    if (address instanceof Inet6Address && address.isLinkLocalAddress()) {
                        interfaceIndex = ni.getIndex(); sourceHex = hex(address.getAddress());
                        sourceText = InetAddress.getByAddress(address.getAddress()).getHostAddress();
                        log("interface=" + IFACE + " index=" + interfaceIndex + " source=" + sourceText);
                        log("MOBILE_MOSEY_READY"); return;
                    }
                }
            }
            pause(200);
        } while (System.nanoTime() < end);
        throw new IOException("Mosey interface has no link-local source");
    }
    boolean knownPeer() throws Exception {
        String text = required(false, IP, "-6", "neigh", "show", "dev", IFACE).text;
        for (String line : text.split("\\r?\\n")) {
            String[] words = line.trim().split("\\s+");
            if (words.length < 3 || !words[0].contains(":")) continue;
            for (int i = 1; i + 1 < words.length; i++) {
                if (words[i].equals("lladdr") && words[i + 1].equalsIgnoreCase(PEER_MAC)) {
                    String literal = words[0].split("%", 2)[0];
                    if (hex(InetAddress.getByName(literal).getAddress()).equals(PEER_HEX)) return true;
                }
            }
        }
        return false;
    }
    void installRoute() throws Exception {
        String rules = required(false, IP, "-6", "rule", "show").text;
        for (String line : rules.split("\\r?\\n")) {
            if (line.trim().startsWith(PRIORITY + ":") || line.matches(".*\\b(?:lookup|table)\\s+" + TABLE + "(?:\\s.*|$)"))
                throw new IOException("temporary route rule already in use");
        }
        if (!required(false, IP, "-6", "route", "show", "table", TABLE).text.trim().isEmpty())
            throw new IOException("temporary route table already in use");
        // Finish each small mutation before observing cancellation so ownership is recorded.
        check(); required(true, IP, "-6", "route", "add", PEER + "/128", "dev", IFACE, "table", TABLE); routeAdded = true;
        check(); required(true, IP, "-6", "rule", "add", "priority", PRIORITY, "to", PEER + "/128", "oif", IFACE, "uidrange", "0-0", "table", TABLE); ruleAdded = true;
        String got = required(false, IP, "-6", "route", "get", PEER, "from", sourceText, "oif", IFACE).text;
        if (!got.matches("(?s).*\\bdev\\s+" + IFACE + "\\b.*") || !got.matches("(?s).*\\btable\\s+" + TABLE + "\\b.*"))
            throw new IOException("scoped route verification failed: " + got.trim());
        log("step=route_ready table=" + TABLE + " priority=" + PRIORITY);
    }
    void clearRoute() throws Exception {
        Exception failure = null;
        if (ruleAdded) {
            try { required(true, IP, "-6", "rule", "del", "priority", PRIORITY, "to", PEER + "/128", "oif", IFACE, "uidrange", "0-0", "table", TABLE); ruleAdded = false; }
            catch (Exception e) { failure = e; }
        }
        if (routeAdded) {
            try { required(true, IP, "-6", "route", "del", PEER + "/128", "dev", IFACE, "table", TABLE); routeAdded = false; }
            catch (Exception e) {
                // The kernel may remove the owned route when Mosey destroys its interface.
                try {
                    if (required(true, IP, "-6", "route", "show", "table", TABLE).text.trim().isEmpty()) routeAdded = false;
                    else if (failure == null) failure = e;
                } catch (Exception checkError) { if (failure == null) failure = e; }
            }
        }
        if (failure != null) throw failure;
        log("step=route_cleanup_ok");
    }
    void transfer() throws Exception {
        check();
        if (!required(false, "/system/bin/id", "-u").text.trim().equals("0")) throw new IOException("root helper required");
        start(); obtainInterface();
        long discoveryEnd = Math.min(deadline, System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(DISCOVERY_MS));
        log("step=peer_discovery");
        while (!knownPeer()) {
            if (System.nanoTime() >= discoveryEnd) throw new IOException("known iPhone not discovered");
            pause(400);
        }
        log("peer_verified=" + PEER_MAC + " ipv6=" + PEER);
        installRoute();
        log("step=airdrop_discovery");
        int port = -1;
        while (System.nanoTime() < discoveryEnd && port < 0) {
            check();
            Result r = command(false, 8000, new File(base, "service-probe").getPath(), "mdns", IFACE,
                Integer.toString(interfaceIndex), sourceHex, PEER_HEX, "5353");
            log("mdns_exit=" + r.code);
            List<Record> records = new ArrayList<>();
            for (String line : r.text.split("\\r?\\n")) {
                if (!line.startsWith("dns_response_hex=")) continue;
                try { records.addAll(decodeDns(line.substring("dns_response_hex=".length()).trim())); }
                catch (IOException e) { log("dns_rejected=" + e.getMessage()); }
            }
            if (r.code == 0) port = airdropPort(records);
            if (port < 0) pause(300);
        }
        if (port < 0) throw new IOException("no verified _airdrop._tcp.local service for known iPhone");
        check(); log("step=transfer port=" + port);
        StreamingTransferProbe.run(new String[]{IFACE, Integer.toString(interfaceIndex), sourceHex, PEER_HEX,
            Integer.toString(port), new File(run, "identity.p12").getPath(), run.getPath()});
        check(); log("step=transfer_complete");
    }

    static String hex(byte[] value) {
        char[] digits = "0123456789abcdef".toCharArray(), out = new char[value.length * 2];
        for (int i = 0; i < value.length; i++) { out[i * 2] = digits[(value[i] & 255) >>> 4]; out[i * 2 + 1] = digits[value[i] & 15]; }
        return new String(out);
    }
    static byte[] unhex(String value) throws IOException {
        if ((value.length() & 1) != 0 || value.length() > 131070) throw new IOException("invalid DNS hex length");
        byte[] result = new byte[value.length() / 2];
        for (int i = 0; i < result.length; i++) {
            int a = Character.digit(value.charAt(i * 2), 16), b = Character.digit(value.charAt(i * 2 + 1), 16);
            if (a < 0 || b < 0) throw new IOException("invalid DNS hex"); result[i] = (byte) ((a << 4) | b);
        }
        return result;
    }
    static int u16(byte[] b, int p) throws IOException {
        if (p < 0 || p + 2 > b.length) throw new IOException("truncated DNS integer");
        return ((b[p] & 255) << 8) | (b[p + 1] & 255);
    }
    static final class Name {
        final List<String> labels; final int next;
        Name(List<String> labels, int next) { this.labels = labels; this.next = next; }
    }
    static Name name(byte[] data, int offset, int limit) throws IOException {
        if (offset < 0 || offset >= limit || limit > data.length) throw new IOException("DNS name outside field");
        List<String> labels = new ArrayList<>(); Set<Integer> seen = new HashSet<>();
        int p = offset, next = -1, expanded = 1; boolean jumped = false;
        for (int hops = 0; hops < 256; hops++) {
            int bound = jumped ? data.length : limit;
            if (p >= bound || !seen.add(p)) throw new IOException("DNS name loop or truncation");
            int length = data[p] & 255;
            if ((length & 0xc0) == 0xc0) {
                if (p + 2 > bound) throw new IOException("truncated DNS pointer");
                int target = ((length & 63) << 8) | (data[p + 1] & 255);
                if (target >= p || target >= data.length) throw new IOException("DNS pointer not backward");
                if (next < 0) next = p + 2;
                p = target; jumped = true; continue;
            }
            if ((length & 0xc0) != 0) throw new IOException("unsupported DNS label");
            p++;
            if (length == 0) return new Name(labels, next < 0 ? p : next);
            if (p + length > bound || (expanded += length + 1) > 255 || labels.size() >= 127)
                throw new IOException("DNS label exceeds bounds");
            // Preserve arbitrary label bytes; lowercase ASCII only. Separate labels prevent dot ambiguity.
            StringBuilder label = new StringBuilder();
            for (int i = 0; i < length; i++) { int c = data[p + i] & 255; label.append((char) (c >= 65 && c <= 90 ? c + 32 : c)); }
            labels.add(label.toString()); p += length;
        }
        throw new IOException("excessive DNS compression");
    }
    static final class Record {
        List<String> owner, target; int type, port; long ttl; String address;
    }
    static List<Record> decodeDns(String text) throws IOException {
        byte[] data = unhex(text);
        if (data.length < 12) throw new IOException("short DNS message");
        int flags = u16(data, 2), questions = u16(data, 4), count = u16(data, 6) + u16(data, 8) + u16(data, 10);
        if (u16(data, 0) != 0 || (flags & 0x8000) == 0 || (flags & 0x7a0f) != 0 || questions > 64 || count > 512)
            throw new IOException("not a complete successful mDNS response");
        int offset = 12; List<Record> records = new ArrayList<>();
        for (int i = 0; i < questions; i++) {
            offset = name(data, offset, data.length).next;
            if (offset + 4 > data.length) throw new IOException("truncated DNS question"); offset += 4;
        }
        for (int i = 0; i < count; i++) {
            Name owner = name(data, offset, data.length); offset = owner.next;
            if (offset + 10 > data.length) throw new IOException("truncated DNS RR");
            Record r = new Record(); r.owner = owner.labels; r.type = u16(data, offset);
            int cls = u16(data, offset + 2) & 0x7fff;
            r.ttl = ((long) u16(data, offset + 4) << 16) | u16(data, offset + 6);
            int length = u16(data, offset + 8); offset += 10; int end = offset + length;
            if (end > data.length) throw new IOException("DNS RDATA out of bounds");
            if (r.type == 12 || r.type == 33) {
                int start = offset;
                if (r.type == 33) {
                    if (length < 7) throw new IOException("short SRV"); r.port = u16(data, offset + 4); start += 6;
                }
                Name target = name(data, start, end);
                if (target.next != end) throw new IOException("name does not fill RDATA"); r.target = target.labels;
            } else if (r.type == 28) {
                if (length != 16) throw new IOException("invalid AAAA length"); r.address = hex(Arrays.copyOfRange(data, offset, end));
            } else if (r.type == 16) {
                int p = offset;
                while (p < end) { int size = data[p++] & 255; p += size; if (p > end) throw new IOException("truncated TXT"); }
            }
            if (cls == 1 && r.ttl > 0) records.add(r);
            offset = end;
        }
        if (offset != data.length) throw new IOException("trailing DNS data");
        return records;
    }
    static int airdropPort(List<Record> records) {
        List<String> serviceName = Arrays.asList("_airdrop", "_tcp", "local");
        Set<List<String>> instances = new HashSet<>();
        for (Record r : records) if (r.type == 12 && r.owner.equals(serviceName) && r.target.size() == 4
            && r.target.subList(1, 4).equals(serviceName)) instances.add(r.target);
        for (Record r : records) {
            if (r.type != 33 || r.port == 0 || !instances.contains(r.owner) || r.target.size() < 2
                || !r.target.get(r.target.size() - 1).equals("local")) continue;
            for (Record address : records) if (address.type == 28 && address.owner.equals(r.target) && PEER_HEX.equals(address.address))
                return r.port;
        }
        return -1;
    }

    public static void main(String[] args) {
        MobileSession session = null; Thread timer = null; Throwable failure = null;
        try {
            if (args.length != 2) throw new IllegalArgumentException("MobileSession BASE RUN");
            session = new MobileSession(args[0], args[1]); timer = session.watchdog(Thread.currentThread());
            session.transfer();
        } catch (Throwable e) { failure = e; log("session_error=" + e); }
        finally {
            // Cleanup must execute even after cancellation/deadline interrupted a blocking command.
            if (session != null) {
                session.finished = true; if (timer != null) timer.interrupt();
                Thread.interrupted();
                try { session.clearRoute(); } catch (Throwable e) { log("route_cleanup_error=" + e); if (failure == null) failure = e; }
                try { session.stop(); } catch (Throwable e) { log("mosey_stop_error=" + e); if (failure == null) failure = e; }
            }
        }
        if (failure == null) { log("MOBILE_SESSION_COMPLETE"); log("mobile_session_exit=0"); }
        else { log("MOBILE_SESSION_FAILED"); log("mobile_session_exit=1"); failure.printStackTrace(System.out); System.out.flush(); System.exit(1); }
    }
}
