import android.app.ActivityManager;
import android.content.ComponentName;
import android.content.Context;
import android.os.Looper;
import android.os.PowerManager;
import java.util.List;

/** Narrow root-only Binder snapshot: no intent extras, filenames or screen text. */
final class RootForeground {
  static final String GMS="com.google.android.gms";
  static final String MAIN=GMS+".nearby.sharing.main.MainActivity";
  final Context context;
  private final ActivityManager activity;
  private final PowerManager power;
  private final Object window;
  private final java.lang.reflect.Method keyguardLocked;
  RootForeground() throws Exception {
    if(android.os.Process.myUid()!=0)throw new SecurityException("ROOT_REQUIRED");
    Looper.prepareMainLooper();
    Class<?> at=Class.forName("android.app.ActivityThread");
    Object thread=at.getMethod("systemMain").invoke(null);
    context=(Context)at.getMethod("getSystemContext").invoke(thread);
    activity=(ActivityManager)context.getSystemService(Context.ACTIVITY_SERVICE);
    power=(PowerManager)context.getSystemService(Context.POWER_SERVICE);
    // KeyguardManager initializes WindowManagerGlobal shared memory, which a
    // standalone root app_process does not own. Query its Binder directly.
    Object binder=Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,"window");
    window=Class.forName("android.view.IWindowManager$Stub").getMethod("asInterface",android.os.IBinder.class).invoke(null,binder);
    keyguardLocked=Class.forName("android.view.IWindowManager").getMethod("isKeyguardLocked");
  }
  static final class Snapshot {
    String component="";int task=-1,currentUser=-1,taskUser=-1;
    boolean interactive,locked,quickShare;
    String json(){return "{\"quickShare\":"+quickShare+",\"component\":\""+component+"\",\"task\":"+task+",\"currentUser\":"+currentUser+",\"taskUser\":"+taskUser+",\"interactive\":"+interactive+",\"locked\":"+locked+"}";}
  }
  Snapshot snapshot() throws Exception {
    Snapshot s=new Snapshot();
    s.currentUser=(Integer)ActivityManager.class.getDeclaredMethod("getCurrentUser").invoke(null);
    s.interactive=power.isInteractive();s.locked=(Boolean)keyguardLocked.invoke(window);
    List<ActivityManager.RunningTaskInfo> tasks=activity.getRunningTasks(1);
    if(!tasks.isEmpty()){
      ActivityManager.RunningTaskInfo t=tasks.get(0);ComponentName top=t.topActivity;
      s.task=t.id;s.taskUser=(Integer)t.getClass().getField("userId").get(t);
      if(top!=null){s.component=top.flattenToShortString();
        s.quickShare=s.currentUser==0&&s.taskUser==0&&s.interactive&&!s.locked&&GMS.equals(top.getPackageName())&&MAIN.equals(top.getClassName());}
    }
    return s;
  }
}
