#!/system/bin/sh
# Fresh-install transaction only. Never writes Android partitions or starts radio.
set -eu
PATH=/system/bin:/system/xbin
export PATH LC_ALL=C
STAGE=${1:-}
[ "$STAGE" = /data/adb/modules_update/houji_qs_system79 ] || exit 2
[ -d "$STAGE" ] && [ ! -L "$STAGE" ] || exit 2
. "$STAGE/common.sh"
# Common state helpers use the staged dex until KSU commits this directory.
MOD=$STAGE
binder_cmd() {
    if binder_output=$(/system/bin/timeout -s TERM -k 2 15 "$@" </dev/null 2>&1);then binder_rc=0;else binder_rc=$?;fi
    [ -z "$binder_output" ] || printf '%s\n' "$binder_output"
    return "$binder_rc"
}
. "$STAGE/profile.env"
[ "$(id -u)" = 0 ] || fail NEED_ROOT
[ "$(getprop sys.boot_completed)" = 1 ] || fail ANDROID_NOT_READY
[ ! -e /data/adb/modules/houji_qs_system79 ] || fail FRESH_INSTALL_ONLY
[ ! -e "$META" ] || fail EXISTING_METADATA_REQUIRES_REVIEW
[ ! -e /data/adb/service.d/houji-qs-system79-finalize.sh ] || fail CLEANUP_PATH_EXISTS
# A clean KSU install creates watchdog/ksu only after the first module commit.
# Validate its parent, and let KSU generate the final rc after staging succeeds.
[ -d /metadata/watchdog ] && [ ! -L /metadata/watchdog ] && [ -w /metadata/watchdog ] || fail PREINIT_PARENT_UNAVAILABLE
[ -x /system/bin/nsenter ] && [ -x /system/bin/timeout ] || fail REQUIRED_TOOLS_MISSING
radio_idle || fail STOP_EXISTING_SESSION_FIRST
[ "$(getprop ro.product.device)" = "$PROFILE_DEVICE" ] || fail DEVICE_CHANGED
[ "$(getprop ro.boot.slot_suffix)" = "$PROFILE_SLOT" ] || fail SLOT_CHANGED
[ "$(uname -r)" = "$PROFILE_KERNEL" ] || fail KERNEL_CHANGED
[ "$(getprop ro.build.fingerprint)" = "$PROFILE_FINGERPRINT" ] || fail BUILD_CHANGED
[ "$(getprop ro.vendor_dlkm.build.version.incremental)" = "$PROFILE_INCREMENTAL" ] || fail INCREMENTAL_CHANGED
[ "$(getenforce)" = Enforcing ] || fail SELINUX_CHANGED
echo "$PROFILE_KSUD_SHA  $KSUD" | sha256sum -c - || fail KSUD_CHANGED
(cd "$STAGE" && sha256sum -c installation-assets.sha256) || fail ASSET_HASH
sha256sum -c "$STAGE/profile-device.sha256" || fail RADIO_BASELINE_CHANGED
[ ! -e /system_ext/priv-app/MoseyApp ] || fail PRIVAPP_COLLISION
grep -q '[[:space:]]overlay$' /proc/filesystems || fail NO_OVERLAYFS
check_tree /system_ext/etc/vintf/manifest "$STAGE/metadata-template/baseline/vintf" || fail VINTF_BASELINE
check_tree /system_ext/etc/permissions "$STAGE/baseline/permissions" || fail PERMISSIONS_BASELINE
check_tree /system_ext/etc/default-permissions "$STAGE/baseline/default-permissions" || fail DEFAULT_PERMISSIONS_BASELINE
echo "$(cat "$STAGE/baseline/service-contexts.sha256")  /system_ext/etc/selinux/system_ext_service_contexts" | sha256sum -c - || fail CONTEXT_BASELINE
"$KSUD" sepolicy check "$(cat "$STAGE/policy.candidate")" || fail POLICY_SYNTAX

package_matches() {
    found=$(binder_cmd pm path "$1") || return 1
    path=$(printf '%s\n' "$found" | sed -n 's/^package://p')
    [ -n "$path" ] && [ "$(printf '%s\n' "$path" | wc -l)" = 1 ] && [ -f "$path" ] || return 1
    echo "$2  $path" | sha256sum -c - >/dev/null
}
package_exists() {
    list=$(binder_cmd pm list packages -u) || fail PACKAGE_LIST_FAILED
    printf '%s\n' "$list" | grep -qx "package:$1"
}
# Preflight BOTH packages before installing either; never replace another build.
for item in google companion; do
    case "$item" in
      google) pkg=com.google.android.mosey; digest=$PROFILE_GOOGLE_SHA ;;
      companion) pkg=org.houji.airdrop; digest=$PROFILE_APP_SHA ;;
    esac
    if package_exists "$pkg"; then package_matches "$pkg" "$digest" || fail "EXISTING_PACKAGE_CHANGED_$item";fi
done

mkdir -m 0700 "$META"
exec >"$META/install.log" 2>&1
COMMITTED=0
finish_install() {
    install_exit_code=$?;trap - EXIT
    if [ "$COMMITTED" != 1 ]; then
        : >"$META/DISABLED"
        if [ -f "$META/state-before.tsv" ] && [ "$(state_value "$META/state-before.tsv" COMPONENT)" = 0 ] && package_matches com.google.android.mosey "$PROFILE_GOOGLE_SHA";then
            binder_cmd pm default-state --user 0 "$COMP" || true
        fi
        # Only packages absent before THIS transaction may be removed, and only
        # while their exact bytes still match. Unknown/replaced apps are retained.
        for item in companion google; do
            case "$item" in
              google) pkg=com.google.android.mosey;digest=$PROFILE_GOOGLE_SHA ;;
              companion) pkg=org.houji.airdrop;digest=$PROFILE_APP_SHA ;;
            esac
            if [ -f "$META/package-created-$item" ]; then
                if package_matches "$pkg" "$digest"; then
                    binder_cmd /system/bin/timeout -s TERM -k 2 20 pm uninstall --user 0 "$pkg" || true
                else echo "FAILED_INSTALL_PACKAGE_PRESERVED=$pkg";fi
            fi
        done
        echo INSTALL_FAILED_DISABLED
    fi
    exit "$install_exit_code"
}
trap finish_install EXIT
printf '%s\n' houji_qs_system79 >"$META/owner.txt"
: >"$META/DISABLED"
cp -R "$STAGE/metadata-template/." "$META/"
cp "$STAGE/profile.env" "$STAGE/profile.json" "$META/"
mkdir -m 0700 "$META/rollback"
cp "$STAGE/installer/finalize.sh" "$STAGE/installer/finalize-launcher.sh" "$STAGE/common.sh" "$STAGE/state-classes.dex" "$STAGE/country-lease.sh" "$META/rollback/"
cp "$STAGE/installer/rollback-assets.sha256" "$META/rollback/"
chmod 0600 "$META/rollback/"*
chcon -R u:object_r:metadata_file:s0 "$META"
chcon -R u:object_r:system_file:s0 "$META/vintf"
chmod 0755 "$META/early-once.sh" "$META/bind-ro"

for item in google companion; do
    case "$item" in
      google) pkg=com.google.android.mosey;digest=$PROFILE_GOOGLE_SHA;apk="$STAGE/payload/priv-app/MoseyApp/MoseyApp.apk" ;;
      companion) pkg=org.houji.airdrop;digest=$PROFILE_APP_SHA;apk="$STAGE/companion.apk" ;;
    esac
    if package_exists "$pkg"; then
        package_matches "$pkg" "$digest" || fail PACKAGE_CHANGED_DURING_INSTALL
        echo "$pkg" >"$META/package-preexisting-$item"
    else
        # Journal intent first, so loss of this shell cannot leave an unrecorded APK.
        echo "$pkg" >"$META/package-created-$item";sync
        size=$(stat -c %s "$apk")
        # PackageManager receives anonymous pipes, never a metadata_file log FD.
        # Keep the APK stdin stream; binder_cmd intentionally uses /dev/null and
        # therefore is not suitable for this streaming operation.
        if install_output=$(cat "$apk" | /system/bin/timeout -s TERM -k 2 60 pm install --user 0 -S "$size" 2>&1);then
            install_rc=0
        else install_rc=$?;fi
        [ -z "$install_output" ] || printf '%s\n' "$install_output"
        [ "$install_rc" = 0 ] || fail PACKAGE_INSTALL_COMMAND_FAILED
        package_matches "$pkg" "$digest" || fail PACKAGE_INSTALL_NOT_CONFIRMED
    fi
done
state_to "$META/state-before.tsv" || fail STATE_QUERY
[ "$(state_value "$META/state-before.tsv" COMPONENT)" = 0 ] || fail COMPONENT_BASELINE_CHANGED
[ "$(state_value "$META/state-before.tsv" SYSTEM_APP)" = 0 ] || fail ALREADY_SYSTEM_APP
[ "$(state_value "$META/state-before.tsv" DECLARED)" = 0 ] || fail ALREADY_DECLARED
boot_id >"$META/install.boot_id"
binder_cmd am force-stop "$PKG"
binder_cmd pm disable --user 0 "$COMP"
# The manager owns modules_update -> modules and initrc regeneration. Do not
# create/enable an active module here. No permission grants or radio autostart.
rm "$META/DISABLED"
: >"$META/ENABLED"
sync
COMMITTED=1
echo STAGED_INSTALL_COMPLETE_REBOOT_REQUIRED
