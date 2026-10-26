import android.content.ComponentName;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Looper;
import android.os.UserHandle;
import java.lang.reflect.Method;

/** Root-only state query; no grants, settings, package or radio mutations. */
public final class IntegrationState {
  public static void main(String[] args) {
    try {
      Looper.prepareMainLooper();
      Class<?> at=Class.forName("android.app.ActivityThread");
      Object thread=at.getMethod("systemMain").invoke(null);
      Context ctx=(Context)at.getMethod("getSystemContext").invoke(thread);
      PackageManager pm=ctx.getPackageManager();
      String pkg="com.google.android.mosey";
      ApplicationInfo info=pm.getApplicationInfo(pkg,0);
      Method flags=PackageManager.class.getMethod("getPermissionFlags",String.class,String.class,UserHandle.class);
      UserHandle user=(UserHandle)UserHandle.class.getMethod("of",int.class).invoke(null,0);
      System.out.println("COMPONENT|"+pm.getComponentEnabledSetting(new ComponentName(pkg,pkg+".ExternalSharingService")));
      System.out.println("SYSTEM_APP|"+((info.flags&ApplicationInfo.FLAG_SYSTEM)!=0?1:0));
      boolean declared=(Boolean)Class.forName("android.os.ServiceManager").getMethod("isDeclared",String.class)
          .invoke(null,"com.google.android.moseyservice.IMoseyService/default");
      System.out.println("DECLARED|"+(declared?1:0));
      for(String name:new String[]{"ACCESS_COARSE_LOCATION","ACCESS_FINE_LOCATION","BLUETOOTH_SCAN",
          "BLUETOOTH_ADVERTISE","NEARBY_WIFI_DEVICES","ACCESS_LOCAL_NETWORK"}) {
        String permission="android.permission."+name;
        int f=(Integer)flags.invoke(pm,permission,pkg,user);
        System.out.println("P|"+name+"|"+(pm.checkPermission(permission,pkg)==PackageManager.PERMISSION_GRANTED?1:0)+"|"+f);
      }
      System.out.println("STATE_COMPLETE|1");
      System.exit(0);
    } catch(Throwable t) {t.printStackTrace();System.exit(1);}
  }
}
