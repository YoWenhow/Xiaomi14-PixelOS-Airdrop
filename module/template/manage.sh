#!/system/bin/sh
# Root management API. Reboot is explicit and remains the caller's action.
set -eu
MODDIR=/data/adb/modules/houji_qs_system79
. "$MODDIR/common.sh"
[ "$(id -u)" = 0 ] || fail NEED_ROOT
ACTION=${1:-status}
ENABLE_ATTEMPT=0 ENABLE_OK=0
if [ "$ACTION" != status ]; then
    mkdir "$RAM-management-lock" 2>/dev/null || fail MANAGEMENT_BUSY
    manage_exit() {
        rc=$?;trap - EXIT
        if [ "$ENABLE_ATTEMPT" = 1 ] && [ "$ENABLE_OK" != 1 ]; then disable_persistently;fi
        rmdir "$RAM-management-lock" 2>/dev/null || true
        exit "$rc"
    }
    trap manage_exit EXIT
fi
current_apk_matches() {
    APK=$(binder_cmd pm path "$PKG" | sed -n 's/^package://p')
    [ -n "$APK" ] && [ -f "$APK" ] || return 1
    echo "$(cat "$META/expected-apk.sha256")  $APK" | sha256sum -c - >/dev/null
}
require_ready() {
    [ -f "$META/ENABLED" ] && [ ! -e "$META/DISABLED" ] && [ ! -e "$MOD/disable" ] && [ ! -e "$MOD/remove" ] || fail INTEGRATION_DISABLED
    [ -f "$META/boot-ready.boot_id" ] && [ "$(cat "$META/boot-ready.boot_id")" = "$(boot_id)" ] || fail BOOT_GUARD_NOT_READY
    [ -f "$META/postfs-success.boot_id" ] && [ "$(cat "$META/postfs-success.boot_id")" = "$(boot_id)" ] || fail POSTFS_NOT_READY
    check_build
    current_apk_matches || fail INSTALLED_APK_CHANGED
}
disable() {
    radio_idle || fail STOP_RADIO_WITH_APP_FIRST
    : >"$META/DISABLED"
    boot_id >"$META/disable.boot_id"
    disable_persistently
    binder_cmd am force-stop "$PKG"
    binder_cmd pm disable --user 0 "$COMP"
    echo DISABLED_REBOOT_THEN_RUN_AFTER_REBOOT_ROLLBACK
}
case "$ACTION" in
status)
    echo "BOOT_ID=$(boot_id)"
    if [ -f "$META/ENABLED" ] && [ ! -e "$META/DISABLED" ] && [ ! -e "$MOD/disable" ] && [ ! -e "$MOD/remove" ]; then echo SYSTEMIZATION_ENABLED=1; else echo SYSTEMIZATION_ENABLED=0; fi
    if [ -f "$META/boot-ready.boot_id" ] && [ "$(cat "$META/boot-ready.boot_id")" = "$(boot_id)" ]; then echo BOOT_READY=1;else echo BOOT_READY=0;fi
    [ ! -e "$META/failure.txt" ] || cat "$META/failure.txt"
    ;;
enable)
    radio_idle || fail STOP_RADIO_WITH_APP_FIRST
    check_build
    current_apk_matches || fail INSTALLED_APK_CHANGED
    state_to "$META/enable-check.tsv" || fail STATE_QUERY
    [ "$(state_value "$META/enable-check.tsv" SYSTEM_APP)" = 0 ] && [ "$(state_value "$META/enable-check.tsv" DECLARED)" = 0 ] || fail FULL_REBOOT_TO_BASE_REQUIRED
    check_tree /system_ext/etc/vintf/manifest "$META/baseline/vintf" || fail VINTF_BASELINE
    check_tree /system_ext/etc/permissions "$MOD/baseline/permissions" || fail PERMISSIONS_BASELINE
    check_tree /system_ext/etc/default-permissions "$MOD/baseline/default-permissions" || fail DEFAULT_PERMISSIONS_BASELINE
    (cd "$META" && sha256sum -c early-assets.sha256) || fail EARLY_ASSET_HASH
    (cd "$MOD" && sha256sum -c module-assets.sha256) || fail MODULE_ASSET_HASH
    ENABLE_ATTEMPT=1
    : >"$META/DISABLED"
    binder_cmd am force-stop "$PKG"
    binder_cmd pm disable --user 0 "$COMP"
    # Explicit enable is the only action that may clear an interrupted-attempt gate.
    if [ -d "$META/INFLIGHT" ]; then
        [ -f "$META/INFLIGHT/boot_id" ] || fail UNKNOWN_INFLIGHT_CONTENTS
        rm "$META/INFLIGHT/boot_id";rmdir "$META/INFLIGHT"
    fi
    rm -f "$META/failure.txt"
    "$KSUD" module enable "$ID"
    "$KSUD" initrc refresh
    grep -Fq "$META/early-once.sh" /metadata/watchdog/ksu/modules.rc || fail PREINIT_NOT_GENERATED
    : >"$META/ENABLED"
    rm "$META/DISABLED"
    sync
    ENABLE_OK=1
    echo ENABLED_EACH_BOOT_REBOOT_REQUIRED
    ;;
grant-runtime)
    require_ready
    state_to "$META/grant-current.tsv" || fail STATE_QUERY
    for perm in $PERMS; do
        granted=$(permission_value "$META/grant-current.tsv" "$perm" 3)
        flags=$(permission_value "$META/grant-current.tsv" "$perm" 4)
        [ "$granted" = 0 ] || { echo "ALREADY_GRANTED=$perm";continue; }
        before=$(permission_value "$META/state-before.tsv" "$perm" 3)
        # Journal before mutation, so a killed grant cannot become untracked.
        if [ "$before" = 0 ]; then echo "$flags" >"$META/permission-pending-$perm";sync;fi
        binder_cmd pm grant --user 0 "$PKG" "android.permission.$perm"
        state_to "$META/grant-after.tsv" || fail GRANT_STATE_QUERY
        [ "$(permission_value "$META/grant-after.tsv" "$perm" 3)" = 1 ] || fail GRANT_NOT_EFFECTIVE
        if [ "$before" = 0 ]; then
            permission_value "$META/grant-after.tsv" "$perm" 4 >"$META/permission-owned-$perm.tmp"
            mv "$META/permission-owned-$perm.tmp" "$META/permission-owned-$perm"
            rm -f "$META/permission-pending-$perm";sync
        fi
        echo "GRANTED=$perm"
    done
    ;;
disable) disable ;;
uninstall)
    radio_idle || fail STOP_RADIO_WITH_APP_FIRST
    : >"$META/UNINSTALL_PENDING"
    disable
    ;;
after-reboot-rollback)
    radio_idle || fail RADIO_STILL_ACTIVE
    binder_cmd /system/bin/sh "$MOD/country-lease.sh" recover
    [ "$(getprop sys.boot_completed)" = 1 ] || fail ANDROID_NOT_READY
    [ -f "$META/DISABLED" ] && [ -f "$MOD/disable" ] || fail DISABLE_FIRST
    [ -f "$META/disable.boot_id" ] && [ "$(cat "$META/disable.boot_id")" != "$(boot_id)" ] || fail FULL_REBOOT_REQUIRED
    for view in /proc/self/mountinfo /proc/1/mountinfo; do
        ! grep -Fq houji-qs-system79 "$view" || fail SYSTEMIZATION_MOUNTS_REMAIN
    done
    state_to "$META/rollback-current.tsv" || fail STATE_QUERY
    [ "$(state_value "$META/rollback-current.tsv" SYSTEM_APP)" = 0 ] && [ "$(state_value "$META/rollback-current.tsv" DECLARED)" = 0 ] || fail STILL_SYSTEMIZED
    if current_apk_matches; then
        for perm in $PERMS; do
            before=$(permission_value "$META/state-before.tsv" "$perm" 3)
            current=$(permission_value "$META/rollback-current.tsv" "$perm" 3)
            [ "$before" = 0 ] && [ "$current" = 1 ] || continue
            owner="$META/permission-owned-$perm"
            [ -f "$owner" ] || owner="$META/permission-pending-$perm"
            if [ ! -f "$owner" ]; then echo "PRESERVED_UNOWNED=$perm";continue;fi
            flags=$(permission_value "$META/rollback-current.tsv" "$perm" 4)
            if [ "$flags" != "$(cat "$owner")" ]; then echo "PRESERVED_CHANGED_FLAGS=$perm";continue;fi
            binder_cmd pm revoke --user 0 "$PKG" "android.permission.$perm"
            echo "REVOKED_OWN_GRANT=$perm"
        done
        binder_cmd pm default-state --user 0 "$COMP"
    else
        echo PACKAGE_CHANGED_PERMISSION_AND_COMPONENT_RESTORE_SKIPPED
    fi
    state_to "$META/rollback-after.tsv" || fail FINAL_STATE_QUERY
    # Consume ownership after successful rollback. A later enable must not
    # mistake a user's new grant for this already-finished installation cycle.
    ledger="$META/permission-ledger-$(boot_id)"
    mkdir -p -m 0700 "$ledger"
    for perm in $PERMS; do
        for kind in owned pending; do
            entry="$META/permission-$kind-$perm"
            if [ -f "$entry" ]; then
                [ ! -e "$ledger/permission-$kind-$perm" ] || fail LEDGER_ARCHIVE_COLLISION
                mv "$entry" "$ledger/"
            fi
        done
    done
    sync
    boot_id >"$META/rollback-success.boot_id"
    sync
    echo POST_REBOOT_STATE_ROLLBACK_COMPLETED
    if [ -f "$META/UNINSTALL_PENDING" ]; then
        # Exact owned directory only; preserve metadata audit/permission receipts.
        [ "$MOD" = /data/adb/modules/houji_qs_system79 ] && [ ! -L "$MOD" ] || fail UNSAFE_MODULE_PATH
        grep -qx "id=$ID" "$MOD/module.prop" || fail MODULE_OWNER_CHANGED
        for view in /proc/self/mountinfo /proc/1/mountinfo; do
            ! grep -Fq "$MOD" "$view" || fail MODULE_STILL_REFERENCED_BY_MOUNT
        done
        rm -rf "$MOD"
        "$KSUD" initrc refresh
        ! grep -Fq "$META/early-once.sh" /metadata/watchdog/ksu/modules.rc || fail PREINIT_STILL_REFERENCES_MODULE
        echo UNINSTALLED_INERT_METADATA_AUDIT_RETAINED
    fi
    ;;
*) echo 'Usage: manage.sh status|enable|grant-runtime|disable|uninstall|after-reboot-rollback';exit 2 ;;
esac
