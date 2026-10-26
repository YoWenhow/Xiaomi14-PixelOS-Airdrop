import android.os.IBinder;
import android.os.Parcel;

/** STOP only. Never STARTs a radio, adds a route, or takes ownership of a session. */
public final class QuickShareStopProbe {
    private static final String DESCRIPTOR = "com.google.android.moseyservice.IMoseyService";
    public static void main(String[] args) {
        try {
            if (args.length != 0) throw new IllegalArgumentException("no arguments");
            Class<?> sm = Class.forName("android.os.ServiceManager");
            IBinder binder = (IBinder) sm.getMethod("checkService", String.class)
                    .invoke(null, DESCRIPTOR + "/default");
            if (binder == null || !DESCRIPTOR.equals(binder.getInterfaceDescriptor()))
                throw new IllegalStateException("Mosey Binder absent or wrong descriptor");
            Parcel request = Parcel.obtain();
            Parcel response = Parcel.obtain();
            try {
                request.writeInterfaceToken(DESCRIPTOR);
                request.writeInt(1); // present StopOptions parcelable
                request.writeInt(4); // empty stable AIDL parcelable size
                if (!binder.transact(2, request, response, 0))
                    throw new IllegalStateException("STOP transaction unhandled");
                response.readException();
            } finally {
                request.recycle();
                response.recycle();
            }
            System.out.println("QS_STOP_RPC_OK");
            System.out.flush();
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace(System.out);
            System.out.flush();
            System.exit(1);
        }
    }
}
