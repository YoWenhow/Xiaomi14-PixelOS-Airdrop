import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothManager;
import android.bluetooth.le.AdvertiseCallback;
import android.bluetooth.le.AdvertiseData;
import android.bluetooth.le.AdvertiseSettings;
import android.bluetooth.le.BluetoothLeAdvertiser;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import java.security.SecureRandom;

/** Temporary shell-owned BLE wake; no APK, pairing, scanning, or adapter toggle. */
public final class BleWakeProbe {
    static BluetoothLeAdvertiser advertiser;
    static AdvertiseCallback callback;
    static Handler handler;
    static boolean requested,finished;
    static void finish(int result,String why) {
        if(finished)return;finished=true;
        try {
            if(requested){advertiser.stopAdvertising(callback);System.out.println("stop_requested=true");}
        } catch(Throwable t){t.printStackTrace();result=1;}
        System.out.println("finish="+why+" exit="+result);
        System.out.flush();System.err.flush();System.exit(result);
    }
    public static void main(String[] args) {
        try{run(args);}catch(Throwable error){error.printStackTrace();System.err.flush();System.exit(1);}
    }
    static void run(String[] args) throws Exception {
        if(args.length!=1)throw new IllegalArgumentException("check | duration_seconds 1..45");
        final int seconds=args[0].equals("check")?0:Integer.parseInt(args[0]);
        if(seconds<0||seconds>45)throw new IllegalArgumentException("duration must be 1..45");
        // The launcher must select shell UID before ART opens any Binder handles.
        if(android.os.Process.myUid()!=2000)throw new SecurityException("shell UID required");
        System.out.println("initial_uid="+android.os.Process.myUid());
        Class<?> initializer=Class.forName("android.bluetooth.BluetoothFrameworkInitializer");
        Class<?> serviceManager=Class.forName("android.os.BluetoothServiceManager");
        if(initializer.getMethod("getBluetoothServiceManager").invoke(null)==null)
            initializer.getMethod("setBluetoothServiceManager",serviceManager).invoke(null,serviceManager.getConstructor().newInstance());
        Looper.prepareMainLooper();handler=new Handler(Looper.getMainLooper());
        Class<?> at=Class.forName("android.app.ActivityThread");
        Object thread=at.getDeclaredMethod("systemMain").invoke(null);
        Context system=(Context)at.getDeclaredMethod("getSystemContext").invoke(thread);
        Context context=system.createPackageContext("com.android.shell",0);
        BluetoothManager manager=(BluetoothManager)context.getSystemService(Context.BLUETOOTH_SERVICE);
        BluetoothAdapter adapter=manager.getAdapter();
        if(adapter==null)throw new IllegalStateException("Bluetooth adapter unavailable");
        System.out.println("uid="+android.os.Process.myUid()+" package="+context.getPackageName()+" bluetooth_enabled="+adapter.isEnabled());
        if(!adapter.isEnabled())throw new IllegalStateException("Bluetooth is disabled; not changing settings");
        advertiser=adapter.getBluetoothLeAdvertiser();
        if(advertiser==null)throw new IllegalStateException("advertiser unavailable");
        AdvertiseSettings.Builder builder=new AdvertiseSettings.Builder().setAdvertiseMode(2)
            .setTxPowerLevel(3).setConnectable(false).setDiscoverable(false).setTimeout(Math.max(seconds,1)*1000);
        builder.getClass().getMethod("setOwnAddressType",int.class).invoke(builder,2);
        System.out.println("own_address_type=2 non_resolvable_private=true");
        if(seconds==0){System.out.println("check=OK no_advertising");System.exit(0);return;}
        byte[] payload=new byte[20],nonce=new byte[8];new SecureRandom().nextBytes(nonce);
        payload[0]=5;payload[1]=18;payload[10]=1;System.arraycopy(nonce,0,payload,11,8);
        StringBuilder hex=new StringBuilder();for(byte b:payload)hex.append(String.format("%02x",b&255));
        System.out.println("manufacturer=76 data="+hex+" duration_seconds="+seconds);
        AdvertiseData data=new AdvertiseData.Builder().addManufacturerData(76,payload)
            .addServiceUuid(ParcelUuid.fromString("0000fcf1-0000-1000-8000-00805f9b34fb"))
            .setIncludeDeviceName(false).setIncludeTxPowerLevel(false).build();
        callback=new AdvertiseCallback(){
            @Override public void onStartSuccess(AdvertiseSettings settings){
                System.out.println("advertise_started=true controller_timeout_ms="+settings.getTimeout());
                handler.postDelayed(()->finish(0,"duration_elapsed"),seconds*1000L+200);
            }
            @Override public void onStartFailure(int error){
                System.out.println("advertise_failed="+error);finish(1,"start_failure");
            }
        };
        Runtime.getRuntime().addShutdownHook(new Thread(()->{
            if(requested&&!finished)try{advertiser.stopAdvertising(callback);}catch(Throwable ignored){}
        }));
        handler.postDelayed(()->finish(1,"callback_watchdog"),(seconds+8)*1000L);
        requested=true;
        try{advertiser.startAdvertising(builder.build(),data,callback);Looper.loop();}
        finally{if(!finished)finish(1,"unexpected_exit");}
    }
}
