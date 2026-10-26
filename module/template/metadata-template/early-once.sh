#!/system/bin/sh
set -eu
. /metadata/houji-qs-system79/common.sh
[ -f "$META/ENABLED" ] && [ ! -e "$META/DISABLED" ] || exit 0
# An interrupted attempt is never silently retried on a later boot.
if [ -e "$META/INFLIGHT" ]; then
    : >"$META/DISABLED"
    echo PREVIOUS_BOOT_INCOMPLETE >"$META/failure.txt"
    sync
    exit 0
fi
mkdir "$META/INFLIGHT" || exit 1
boot_id >"$META/INFLIGHT/boot_id"
sync
exec >>"$META/early.log" 2>&1
early_exit() {
    rc=$?;trap - EXIT
    if [ "$rc" -ne 0 ]; then
        : >"$META/DISABLED";echo "EARLY_FAILED=$rc" >"$META/failure.txt";sync
    fi
    exit "$rc"
}
trap early_exit EXIT
echo "EARLY_ATTEMPT boot=$(boot_id)"
(cd "$META" && sha256sum -c early-assets.sha256) || fail EARLY_ASSET_HASH
[ "$(getprop ro.build.fingerprint)" = "$(cat "$META/expected-fingerprint.txt")" ] || fail BUILD_CHANGED
check_tree /system_ext/etc/vintf/manifest "$META/baseline/vintf" || fail VINTF_BASELINE
check_tree "$META/vintf" "$META/candidate-vintf" || fail VINTF_CANDIDATE
bind_ro "$META/vintf" /system_ext/etc/vintf/manifest || fail VINTF_BIND
boot_id >"$META/early-success.boot_id"
echo EARLY_VINTF_OK
