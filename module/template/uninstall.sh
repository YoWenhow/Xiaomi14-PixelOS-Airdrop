#!/system/bin/sh
# KSU invokes this during post-fs-data, before PackageManager is available.
# Its own prune process removes the module directory after this returns.
set -eu
META=/metadata/houji-qs-system79
MOD=${0%/*}
[ "$MOD" = /data/adb/modules/houji_qs_system79 ] && [ ! -L "$MOD" ] || exit 1
[ -d "$META" ] && [ ! -L "$META" ] || exit 1
[ "$(cat "$META/owner.txt")" = houji_qs_system79 ] || exit 1
exec >>"$META/uninstall.log" 2>&1
queue_exit() {
    queue_rc=$?;trap - EXIT
    if [ "$queue_rc" != 0 ];then echo "UNINSTALL_QUEUE_FAILED=$queue_rc" >"$META/rollback-failure.txt";fi
    exit "$queue_rc"
}
trap queue_exit EXIT
: >"$META/DISABLED"
: >"$META/UNINSTALL_PENDING"
cat /proc/sys/kernel/random/boot_id >"$META/uninstall.boot_id"
(cd "$META/rollback" && sha256sum -c rollback-assets.sha256) || exit 1
TARGET=/data/adb/service.d/houji-qs-system79-finalize.sh
mkdir -p /data/adb/service.d
[ ! -L /data/adb/service.d ] || exit 1
# An unknown cleanup hook is never overwritten.
if [ -e "$TARGET" ]; then
    [ ! -L "$TARGET" ] && cmp -s "$TARGET" "$META/rollback/finalize-launcher.sh" || exit 1
else
    (set -C; cat "$META/rollback/finalize-launcher.sh" >"$TARGET") || exit 1
fi
chmod 0700 "$TARGET"
chcon u:object_r:system_file:s0 "$TARGET"
sync
echo UNINSTALL_QUEUED_ONE_SHOT_PERMISSION_ROLLBACK
