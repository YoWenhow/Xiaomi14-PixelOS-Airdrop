/* Diagnostic-only: downgrade this one local Binder before publishing it.
 * The original service and FFI bytes, real transactions and results are retained.
 * Production integration must supply the proper VINTF declaration instead.
 */
typedef struct AIBinder AIBinder;
extern void *dlsym(void *, const char *);
extern void AIBinder_forceDowngradeToSystemStability(AIBinder *);
extern void AServiceManager_forceLazyServicesPersist(_Bool);
static int same(const char *a,const char *b) {
    if(!a||!b)return 0;
    while(*a&&*a==*b){a++;b++;}return *a==*b;
}
int AServiceManager_registerLazyService(AIBinder *binder,const char *instance) {
    typedef int (*Register)(AIBinder *, const char *);
    Register original=(Register)dlsym((void *)-1,"AServiceManager_registerLazyService");
    if(!original)return -38;
    if(same(instance,"com.google.android.moseyservice.IMoseyService/default")) {
        AIBinder_forceDowngradeToSystemStability(binder);
        /* The diagnostic launcher owns the bounded process lifetime. Prevent
         * a transient readiness lookup from causing lazy exit before the probe. */
        AServiceManager_forceLazyServicesPersist(1);
        const char message[]="Stage37: diagnostic Mosey Binder downgraded to system stability\n";
        register long x0 __asm__("x0")=2,x1 __asm__("x1")=(long)message;
        register long x2 __asm__("x2")=sizeof(message)-1,x8 __asm__("x8")=64;
        __asm__ volatile("svc 0":"+r"(x0):"r"(x1),"r"(x2),"r"(x8):"memory","cc");
    }
    return original(binder,instance);
}
