#!/system/bin/sh
set -eu
MODDIR=/data/adb/modules/houji_qs_system79
. "$MODDIR/common.sh"

if [ "${1:-}" != --inner ]; then
    # Capture failures before namespace entry, including an inaccessible script.
    exec >>"$META/outer.log" 2>&1
    echo "POSTFS_OUTER_ATTEMPT argv0=$0 cwd=$(pwd)"
    outer_exit() {
        rc=$?;trap - EXIT
        if [ "$rc" -ne 0 ]; then disable_persistently;echo "POSTFS_OUTER_FAILED=$rc" >"$META/failure.txt";fi
        exit "$rc"
    }
    trap outer_exit EXIT
    echo "caller_ns=$(readlink /proc/self/ns/mnt) init_ns=$(readlink /proc/1/ns/mnt)"
    # Always disable before entering the mount path, even if early-init failed.
    [ ! -e "$MOD/remove" ] || fail MODULE_REMOVAL_PENDING
    : >"$MOD/disable"
    if "$KSUD" initrc refresh >"$META/disable.log" 2>&1; then
        echo INITRC_REFRESH_OK
    else
        rc=$?
        echo "INITRC_REFRESH_FAILED rc=$rc"
        exit "$rc"
    fi
    echo "POSTFS_INNER_ENTRY=$MOD/post-fs-data.sh"
    if /system/bin/timeout -s KILL 20 /system/bin/nsenter -t 1 -m -- /system/bin/sh "$MOD/post-fs-data.sh" --inner; then
        echo POSTFS_INNER_EXIT=0
        [ "$(cat "$META/postfs-success.boot_id")" = "$(boot_id)" ] || fail POSTFS_RECEIPT
        # Only a completely validated mount set may enable another boot.
        [ ! -e "$MOD/remove" ] || fail MODULE_REMOVAL_PENDING
        "$KSUD" module enable "$ID"
        "$KSUD" initrc refresh
        grep -Fq "$META/early-once.sh" /metadata/watchdog/ksu/modules.rc || fail NEXT_PREINIT_MISSING
        [ "$(cat "$META/INFLIGHT/boot_id")" = "$(boot_id)" ] || fail INFLIGHT_BOOT
        rm "$META/INFLIGHT/boot_id"
        rmdir "$META/INFLIGHT"
        sync
        exit 0
    else
        rc=$?
        echo "POSTFS_INNER_EXIT=$rc"
        exit "$rc"
    fi
fi
exec >>"$META/post-fs-data.log" 2>&1
echo POSTFS_ATTEMPT
[ "$(readlink /proc/self/ns/mnt)" = "$(readlink /proc/1/ns/mnt)" ] || fail NOT_INIT_MOUNT_NAMESPACE
[ -f "$MOD/disable" ] || fail NOT_DISABLED
[ -f "$META/ENABLED" ] && [ ! -e "$META/DISABLED" ] || fail NOT_ENABLED
check_build
[ -f "$META/early-success.boot_id" ] || fail NO_EARLY_RECEIPT
[ "$(cat "$META/early-success.boot_id")" = "$(cat /proc/sys/kernel/random/boot_id)" ] || fail STALE_EARLY_RECEIPT
[ ! -e "$RAM" ] || fail RAM_PATH_EXISTS
(cd "$MOD" && sha256sum -c module-assets.sha256) || fail MODULE_ASSET_HASH
check_tree /system_ext/etc/permissions "$MOD/baseline/permissions" || fail PERMISSIONS_BASELINE
check_tree /system_ext/etc/default-permissions "$MOD/baseline/default-permissions" || fail DEFAULT_PERMISSIONS_BASELINE
echo "$(cat "$MOD/baseline/service-contexts.sha256")  /system_ext/etc/selinux/system_ext_service_contexts" | sha256sum -c - || fail SERVICE_CONTEXT_BASELINE
[ ! -e /system_ext/priv-app/MoseyApp ] || fail PRIVAPP_COLLISION
"$KSUD" sepolicy check "$(cat "$MOD/policy.candidate")" || fail POLICY_SYNTAX

mkdir -m 0755 "$RAM"
APP_MOUNT=0 PERM_MOUNT=0 DEFAULT_MOUNT=0 CONTEXT_MOUNT=0 LOWER_MOUNT=0 RAM_MOUNT=0
cleanup_failure() {
    rc=$?
    trap - EXIT
    if [ "$rc" -ne 0 ]; then
        echo "POSTFS_FAILED rc=$rc; module disabled; next full boot restores base"
        disable_persistently
        [ "$CONTEXT_MOUNT" = 0 ] || umount /system_ext/etc/selinux/system_ext_service_contexts || true
        [ "$DEFAULT_MOUNT" = 0 ] || umount /system_ext/etc/default-permissions || true
        [ "$PERM_MOUNT" = 0 ] || umount /system_ext/etc/permissions || true
        [ "$APP_MOUNT" = 0 ] || umount /system_ext/priv-app || true
        [ "$LOWER_MOUNT" = 0 ] || umount "$RAM/priv-app-original" || true
        [ "$RAM_MOUNT" = 0 ] || umount "$RAM" || true
    fi
    exit "$rc"
}
trap cleanup_failure EXIT

# F2FS/fscrypt cannot be an overlay lower. Copy only the added APK into tmpfs.
mount -t tmpfs -o mode=0755,nosuid,nodev,size=64m tmpfs "$RAM"
RAM_MOUNT=1
mkdir -m 0755 "$RAM/priv-app-original" "$RAM/additions" "$RAM/additions/MoseyApp"
cp "$MOD/payload/priv-app/MoseyApp/MoseyApp.apk" "$RAM/additions/MoseyApp/MoseyApp.apk"
chmod 0644 "$RAM/additions/MoseyApp/MoseyApp.apk"
echo "8e087557289d70a6fb29b34fd64c11ff15353e0f07ad89dfebe1dc15e723888a  $RAM/additions/MoseyApp/MoseyApp.apk" | sha256sum -c - || fail RAM_APK_HASH
chcon -R u:object_r:system_file:s0 "$MOD/payload" "$RAM"

# Install all permission metadata before exposing a privileged package to PMS.
bind_ro "$MOD/payload/permissions" /system_ext/etc/permissions
PERM_MOUNT=1
bind_ro "$MOD/payload/default-permissions" /system_ext/etc/default-permissions
DEFAULT_MOUNT=1
bind_ro "$MOD/payload/system_ext_service_contexts" /system_ext/etc/selinux/system_ext_service_contexts
CONTEXT_MOUNT=1
# Applying after the exact context mapping is mounted triggers the SELinux status
# update used by servicemanager to reopen its service-context handle.
"$KSUD" sepolicy apply "$MOD/policy.candidate"
# Lowerdir is a bind of the complete live directory. No original app is copied or hidden.
bind_ro /system_ext/priv-app "$RAM/priv-app-original"
LOWER_MOUNT=1
mount -t overlay overlay -o "ro,nosuid,nodev,lowerdir=$RAM/additions:$RAM/priv-app-original" /system_ext/priv-app
APP_MOUNT=1
readonly_mount_everywhere /system_ext/priv-app || fail FINAL_APP_MOUNT_NOT_READONLY
cat /proc/sys/kernel/random/boot_id >"$META/postfs-success.boot_id"
echo POSTFS_SYSTEMIZATION_OK
# Intentionally no native server, driver, firmware, provider-enable or radio action.
