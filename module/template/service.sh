#!/system/bin/sh
# Late-start guard only: reset provider after an interrupted previous session.
# It never grants permissions or starts native/radio services.
set -eu
MODDIR=/data/adb/modules/houji_qs_system79
. "$MODDIR/common.sh"
exec >>"$META/boot-provider-guard.log" 2>&1
guard_exit() {
    rc=$?;trap - EXIT
    if [ "$rc" -ne 0 ]; then disable_persistently;echo "BOOT_GUARD_FAILED=$rc" >"$META/failure.txt";fi
    exit "$rc"
}
trap guard_exit EXIT
[ -f "$META/ENABLED" ] && [ ! -e "$META/DISABLED" ] || exit 0
[ ! -e "$MOD/disable" ] && [ ! -e "$MOD/remove" ] || exit 0
[ -f "$META/postfs-success.boot_id" ] && [ "$(cat "$META/postfs-success.boot_id")" = "$(boot_id)" ] || fail POSTFS_NOT_READY
n=0
while [ "$(getprop sys.boot_completed)" != 1 ]; do
    n=$((n+1));[ "$n" -le 90 ] || fail BOOT_NOT_COMPLETED
    sleep 1
done
check_build
binder_cmd am force-stop "$PKG"
binder_cmd pm disable --user 0 "$COMP"
binder_cmd /system/bin/sh "$MOD/country-lease.sh" recover
state_to "$META/boot-ready.tsv" || fail BOOT_STATE
[ "$(state_value "$META/boot-ready.tsv" COMPONENT)" = 2 ] || fail PROVIDER_NOT_DISABLED
[ "$(state_value "$META/boot-ready.tsv" SYSTEM_APP)" = 1 ] || fail NOT_SYSTEM_APP
[ "$(state_value "$META/boot-ready.tsv" DECLARED)" = 1 ] || fail NOT_DECLARED
boot_id >"$META/boot-ready.boot_id"
sync
echo BOOT_READY_PROVIDER_DISABLED_NO_RADIO

# Stage80: optional foreground observer, no radio start until an unlocked Quick Share page.
if [ -f "$MOD/auto/enabled" ]; then
    if /system/bin/sh "$MOD/auto/control.sh" start; then :
    else rm -f "$MOD/auto/enabled";echo AUTO_OBSERVER_FAILED_DISABLED;fi
fi
