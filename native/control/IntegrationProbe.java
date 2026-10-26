import android.content.ComponentName;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Looper;
import org.json.JSONObject;

public final class IntegrationProbe {
  public static void main(String[] args) {
    try {
      Looper.prepareMainLooper();
      Class<?> at = Class.forName("android.app.ActivityThread");
      Object thread = at.getMethod("systemMain").invoke(null);
      Context context = (Context)at.getMethod("getSystemContext").invoke(thread);
      PackageManager pm = context.getPackageManager();
      String pkg = "com.google.android.mosey";
      ApplicationInfo info = pm.getApplicationInfo(pkg, 0);
      JSONObject result = new JSONObject();
      result.put("uid", info.uid);
      result.put("source", info.sourceDir);
      result.put("systemApp", (info.flags & ApplicationInfo.FLAG_SYSTEM) != 0);
      result.put("componentEnabledSetting", pm.getComponentEnabledSetting(
          new ComponentName(pkg, pkg + ".ExternalSharingService")));
      result.put("declared", Class.forName("android.os.ServiceManager")
          .getMethod("isDeclared", String.class).invoke(null,
              "com.google.android.moseyservice.IMoseyService/default"));
      JSONObject permissions = new JSONObject();
      for (String name : new String[]{"BLUETOOTH_SCAN", "BLUETOOTH_CONNECT",
          "BLUETOOTH_ADVERTISE", "ACCESS_FINE_LOCATION", "ACCESS_LOCAL_NETWORK",
          "CREATE_APP_SPECIFIC_NETWORK", "MANAGE_WIFI_INTERFACES",
          "MANAGE_WIFI_NETWORK_SELECTION", "NETWORK_FACTORY"}) {
        permissions.put(name, pm.checkPermission("android.permission." + name, pkg)
            == PackageManager.PERMISSION_GRANTED);
      }
      result.put("permissions", permissions);
      System.out.println("INTEGRATION_PROBE=" + result.toString());
      System.exit(0);
    } catch (Throwable t) {
      t.printStackTrace();
      System.exit(1);
    }
  }
}
