/* Narrow static bind helper for Android early-init. No libc or runtime deps. */
typedef unsigned long usize;
typedef unsigned long u64;
#ifdef __aarch64__
enum { READ=63, WRITE=64, CLOSE=57, EXIT=93, OPENAT=56, GETUID=174,
       MOUNT=40, UMOUNT2=39, READLINKAT=78 };
static long syscall6(long n,long a,long b,long c,long d,long e,long f)
{
    register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
    register long x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    register long x8 __asm__("x8")=n;
    __asm__ volatile("svc 0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8):"memory","cc");
    return x0;
}
#else
enum { READ=0, WRITE=1, CLOSE=3, EXIT=60, OPENAT=257, GETUID=102,
       MOUNT=165, UMOUNT2=166, READLINKAT=267 };
static long syscall6(long n,long a,long b,long c,long d,long e,long f)
{
    register long r10 __asm__("r10")=d,r8 __asm__("r8")=e,r9 __asm__("r9")=f;
    long ret;
    __asm__ volatile("syscall":"=a"(ret):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory","cc");
    return ret;
}
#endif
#define AT_FDCWD (-100)
#define MS_RDONLY 1UL
#define MS_NOSUID 2UL
#define MS_NODEV 4UL
#define MS_NOEXEC 8UL
#define MS_REMOUNT 32UL
#define MS_NOSYMFOLLOW 256UL
#define MS_NOATIME 1024UL
#define MS_NODIRATIME 2048UL
#define MS_BIND 4096UL
#define MS_RELATIME (1UL<<21)
#define MS_STRICTATIME (1UL<<24)
#define REQUIRED (MS_RDONLY|MS_NOSUID|MS_NODEV)
static usize slen(const char *s){usize n=0;while(s[n])n++;return n;}
static int eq(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
void *memset(void *d,int v,usize n){unsigned char *p=d;while(n--)*p++=(unsigned char)v;return d;}
void *memcpy(void *d,const void *s,usize n){unsigned char *a=d;const unsigned char *b=s;while(n--)*a++=*b++;return d;}
static void output(const char *s){usize n=slen(s);while(n){long k=syscall6(WRITE,1,(long)s,n,0,0,0);if(k<=0)return;s+=k;n-=k;}}
static void number(long n){char buf[32],*p=buf+31;u64 v;if(n<0){output("-");v=(u64)(-(n+1))+1;}else v=(u64)n;*p=0;do{*--p='0'+v%10;v/=10;}while(v);output(p);}
static void error(const char *step,long code){output("BIND_RO_ERROR step=");output(step);output(" code=");number(code);output("\n");}
static int safe_path(const char *p)
{
    usize n=slen(p);if(n<2||n>=1024||*p!='/')return 0;
    for(;*p;p++)if(*p<33||*p>126||*p=='\\')return 0;
    return 1;
}
static long decimal(const char *s)
{
    unsigned long n=0;if(!*s)return -1;
    for(;*s;s++){if(*s<'0'||*s>'9'||n>214748364UL)return -1;n=n*10+*s-'0';if(n>2147483647UL)return -1;}
    return (long)n;
}
static char *token(char **p)
{
    char *s=*p;if(!*s)return 0;
    while(**p&&**p!=' ')(*p)++;
    if(**p){**p=0;(*p)++;}return s;
}
static unsigned long option_flags(char *s)
{
    unsigned long f=0;
    while(s&&*s){char *p=s;while(*p&&*p!=',')p++;if(*p)*p++=0;
        if(eq(s,"ro"))f|=MS_RDONLY;else if(eq(s,"nosuid"))f|=MS_NOSUID;
        else if(eq(s,"nodev"))f|=MS_NODEV;else if(eq(s,"noexec"))f|=MS_NOEXEC;
        else if(eq(s,"noatime"))f|=MS_NOATIME;else if(eq(s,"nodiratime"))f|=MS_NODIRATIME;
        else if(eq(s,"relatime"))f|=MS_RELATIME;else if(eq(s,"strictatime"))f|=MS_STRICTATIME;
        else if(eq(s,"nosymfollow"))f|=MS_NOSYMFOLLOW;s=p;
    }return f;
}
struct mount_info {long id;unsigned long flags;int count;};
static int parse_line(char *line,const char *target,struct mount_info *info)
{
    char *p=line,*fields[6];int i;for(i=0;i<6;i++)if(!(fields[i]=token(&p)))return -22;
    if(!eq(fields[4],target))return 0;
    long id=decimal(fields[0]);if(id<=0)return -22;
    info->id=id;info->flags=option_flags(fields[5]);info->count++;return 0;
}
static int inspect(const char *target,struct mount_info *info)
{
    char buf[1024],line[16384];usize used=0;int rc=0;long n;
    memset(info,0,sizeof(*info));long fd=syscall6(OPENAT,AT_FDCWD,(long)"/proc/self/mountinfo",02000000,0,0,0);
    if(fd<0)return (int)fd;
    while((n=syscall6(READ,fd,(long)buf,sizeof(buf),0,0,0))>0){long i;for(i=0;i<n;i++){
        if(buf[i]=='\n'){line[used]=0;rc=parse_line(line,target,info);used=0;if(rc)goto done;}
        else {if(used+1>=sizeof(line)){rc=-75;goto done;}line[used++]=buf[i];}
    }}
    if(n<0)rc=(int)n;else if(used)rc=-22;
done: syscall6(CLOSE,fd,0,0,0,0,0);return rc;
}
static long open_path(const char *path,char *canonical,char *fdpath)
{
    char tmp[24],*p=tmp+23;*p=0;long fd=syscall6(OPENAT,AT_FDCWD,(long)path,010000000|02000000,0,0,0);
    if(fd<0)return fd;
    unsigned long n=(unsigned long)fd;do{*--p='0'+n%10;n/=10;}while(n);
    memcpy(fdpath,"/proc/self/fd/",14);memcpy(fdpath+14,p,slen(p)+1);
    long len=syscall6(READLINKAT,AT_FDCWD,(long)fdpath,(long)canonical,1023,0,0);
    if(len<=0||len>=1023){syscall6(CLOSE,fd,0,0,0,0,0);return len<0?len:-22;}
    canonical[len]=0;if(!safe_path(canonical)){syscall6(CLOSE,fd,0,0,0,0,0);return -22;}return fd;
}
static int bind_ro(const char *src,const char *dst)
{
    char source[1024],target[1024],sfdpath[40],dfdpath[40];long sfd=-1,dfd=-1,rc;int status=1,bound=0;
    struct mount_info before,bound_info,after;long mount_id=0;const char *step="arguments";
    if(!safe_path(src)||!safe_path(dst)){error(step,-22);return 1;}
    if(syscall6(GETUID,0,0,0,0,0,0)!=0){error("uid",-1);return 1;}
    step="source";sfd=open_path(src,source,sfdpath);if(sfd<0){rc=sfd;goto done;}
    step="target";dfd=open_path(dst,target,dfdpath);if(dfd<0){rc=dfd;goto done;}
    step="same_path";if(eq(source,target)){rc=-22;goto done;}
    step="mountinfo_before";rc=inspect(target,&before);if(rc)goto done;
    step="target_already_mounted";if(before.count){rc=-16;goto done;}
    step="bind";rc=syscall6(MOUNT,(long)sfdpath,(long)target,0,MS_BIND,0,0);if(rc)goto done;bound=1;
    step="mountinfo_bound";rc=inspect(target,&bound_info);if(rc)goto done;
    if(bound_info.count!=1){rc=-22;goto done;}mount_id=bound_info.id;
    step="remount_bind_ro";
#ifdef BIND_RO_FORCE_REMOUNT_ERROR
    rc=-13;
#else
    rc=syscall6(MOUNT,0,(long)target,0,MS_BIND|MS_REMOUNT|bound_info.flags|REQUIRED,0,0);
#endif
    if(rc)goto done;
    step="mountinfo_readonly";rc=inspect(target,&after);if(rc)goto done;
    if(after.count!=1||after.id!=mount_id||(after.flags&REQUIRED)!=REQUIRED||(after.flags&bound_info.flags)!=bound_info.flags){rc=-22;goto done;}
    output("BIND_RO_OK mount_id=");number(mount_id);output(" target=");output(target);output(" flags=");number((long)after.flags);output("\n");status=0;
done:
    if(status){error(step,rc);if(bound){
        struct mount_info current;long check=inspect(target,&current);
        if(check||current.count!=1||(mount_id&&current.id!=mount_id)){error("rollback_ownership",check?check:-16);status=2;}
        else {long unmount=syscall6(UMOUNT2,(long)target,0,0,0,0,0);if(unmount){error("rollback_umount",unmount);status=2;}
            else {check=inspect(target,&current);if(check||current.count){error("rollback_verify",check?check:-16);status=2;}else output("BIND_RO_ROLLBACK_OK\n");}}
    }}
    if(dfd>=0)syscall6(CLOSE,dfd,0,0,0,0,0);if(sfd>=0)syscall6(CLOSE,sfd,0,0,0,0,0);return status;
}
#ifndef BIND_RO_TEST
static int program(int argc,char **argv)
{
    if(argc==2&&eq(argv[1],"--help")){output("bind-ro SOURCE TARGET: new bind, per-mount readonly/nosuid/nodev, mountinfo verification; rollback on failure.\n");return 0;}
    if(argc!=3){error("usage",-22);return 1;}return bind_ro(argv[1],argv[2]);
}
__attribute__((noreturn,used)) static void entry(long *stack){int rc=program((int)stack[0],(char **)(stack+1));syscall6(EXIT,rc,0,0,0,0,0);__builtin_unreachable();}
#ifdef __aarch64__
__asm__(".global _start\n_start:\nmov x0, sp\nbl entry\n");
#else
__asm__(".global _start\n_start:\nmov %rsp, %rdi\nand $-16, %rsp\ncall entry\n");
#endif
#endif
