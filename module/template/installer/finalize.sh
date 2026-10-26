#!/system/bin/sh
# Standalone, bounded PackageManager rollback; no radio or partition actions.
set -eu
META=/metadata/houji-qs-system79
[ ! -L "$META" ] && [ "$(cat "$META/owner.txt")" = houji_qs_system79 ] || exit 1
exec >>"$META/uninstall-rollback.log" 2>&1
(cd "$META/rollback" && sha256sum -c rollback-assets.sha256) || exit 1
. "$META/rollback/common.sh"
MOD="$META/rollback"
[ -f "$META/DISABLED" ] && [ -f "$META/UNINSTALL_PENDING" ] || fail NOT_UNINSTALLING
cleanup_exit() {
    rc=$?;trap - EXIT
    if [ "$rc" != 0 ]; then echo "ROLLBACK_FAILED=$rc" >"$META/rollback-failure.txt";fi
    # Helpers are inert data; there is no persistent boot hook or retry loop.
    exit "$rc"
}
trap cleanup_exit EXIT
n=0
while [ "$(getprop sys.boot_completed)" != 1 ];do
    n=$((n+1));[ "$n" -le 90 ] || fail BOOT_COMPLETION_TIMEOUT;sleep 2
done
[ ! -d /data/adb/modules/houji_qs_system79 ] || fail MODULE_NOT_REMOVED
[ ! -d /data/adb/modules_update/houji_qs_system79 ] || fail MODULE_UPDATE_REMAINS
radio_idle || fail RADIO_STILL_ACTIVE
binder_cmd /system/bin/sh "$META/rollback/country-lease.sh" recover || fail COUNTRY_LEASE_RESTORE
for view in /proc/self/mountinfo /proc/1/mountinfo;do
    ! grep -Fq houji-qs-system79 "$view" || fail SYSTEMIZATION_MOUNTS_REMAIN
done
# Bound all Binder calls independently as well as the outer launcher deadline.
binder_cmd() {
    if binder_output=$(/system/bin/timeout -s TERM -k 2 12 "$@" </dev/null 2>&1);then binder_rc=0;else binder_rc=$?;fi
    [ -z "$binder_output" ] || printf '%s\n' "$binder_output"
    return "$binder_rc"
}
state_to "$META/rollback-current.tsv" || fail STATE_QUERY
[ "$(state_value "$META/rollback-current.tsv" SYSTEM_APP)" = 0 ] && [ "$(state_value "$META/rollback-current.tsv" DECLARED)" = 0 ] || fail STILL_SYSTEMIZED
apk_output=$(binder_cmd pm path "$PKG") || fail PACKAGE_PATH_QUERY
APK=$(printf '%s\n' "$apk_output" | sed -n 's/^package://p')
if [ -n "$APK" ] && [ "$(printf '%s\n' "$APK" | wc -l)" = 1 ] && [ -f "$APK" ] && echo "$(cat "$META/expected-apk.sha256")  $APK" | sha256sum -c - >/dev/null;then
    for perm in $PERMS;do
        before=$(permission_value "$META/state-before.tsv" "$perm" 3)
        current=$(permission_value "$META/rollback-current.tsv" "$perm" 3)
        [ "$before" = 0 ] && [ "$current" = 1 ] || continue
        owner="$META/permission-owned-$perm"
        [ -f "$owner" ] || owner="$META/permission-pending-$perm"
        if [ ! -f "$owner" ];then echo "PRESERVED_UNOWNED=$perm";continue;fi
        flags=$(permission_value "$META/rollback-current.tsv" "$perm" 4)
        if [ "$flags" != "$(cat "$owner")" ];then echo "PRESERVED_CHANGED_FLAGS=$perm";continue;fi
        binder_cmd pm revoke --user 0 "$PKG" "android.permission.$perm"
        echo "REVOKED_OWN_GRANT=$perm"
    done
    binder_cmd pm default-state --user 0 "$COMP"
else
    echo PACKAGE_CHANGED_PERMISSION_AND_COMPONENT_RESTORE_SKIPPED
fi
state_to "$META/rollback-after.tsv" || fail FINAL_STATE_QUERY
ledger="$META/permission-ledger-$(boot_id)"
mkdir -m 0700 "$ledger"
for perm in $PERMS;do
    for kind in owned pending;do
        entry="$META/permission-$kind-$perm"
        if [ -f "$entry" ];then mv "$entry" "$ledger/";fi
    done
done
boot_id >"$META/rollback-success.boot_id"
sync
echo UNINSTALL_ROLLBACK_COMPLETE_APKS_AND_INERT_AUDIT_RETAINED
