#!/system/bin/sh
ID=houji_qs_system79
META=/metadata/houji-qs-system79
MOD=/data/adb/modules/houji_qs_system79
RAM=/dev/houji-qs-system79
KSUD=/data/adb/ksud
COMP=com.google.android.mosey/.ExternalSharingService
export PATH=/system/bin:/system/xbin
export LC_ALL=C
umask 022

fail() { echo "FAIL: $*" >&2; exit 1; }
# cmd/am/pm/dumpsys pass standard descriptors through Binder. Give the child
# anonymous output pipes and /dev/null input, never a metadata_file log FD.
# The parent shell alone writes the captured text to its current log.
binder_cmd() {
    if binder_output=$("$@" </dev/null 2>&1); then binder_rc=0; else binder_rc=$?; fi
    [ -z "$binder_output" ] || printf '%s\n' "$binder_output"
    return "$binder_rc"
}
inventory() { (cd "$1" && find . -mindepth 1 -print | sort); }
check_tree() {
    [ -d "$1" ] || return 1
    [ -z "$(find "$1" -type l -print)" ] || return 1
    [ "$(inventory "$1")" = "$(cat "$2.inventory")" ] || return 1
    (cd "$1" && sha256sum -c "$2.sha256" >/dev/null)
}
readonly_mount_everywhere() {
    for view in /proc/self/mountinfo /proc/1/mountinfo; do
        awk -v p="$1" '$5==p {count++; if($6 !~ /(^|,)ro(,|$)/ || $6 !~ /(^|,)nosuid(,|$)/ || $6 !~ /(^|,)nodev(,|$)/) bad=1} END {exit count!=1 || bad}' "$view" || return 1
    done
}
make_private() {
    # Explicit type prevents Toybox from autodetecting this as MS_BIND.
    # Linux ignores source/type for MS_PRIVATE; only this mount is changed.
    /system/bin/mount -t tmpfs -o private none "$1" || return 1
    awk -v p="$1" '$5==p {count++; for(i=7;i<=NF && $i!="-";i++) if($i ~ /^(shared|master|propagate_from):/) bad=1} END {exit count!=1 || bad}' /proc/self/mountinfo
}
bind_ro() (
    src=$1 dst=$2
    STAGE=$RAM-staging
    if [ ! -f "$STAGE/owner" ]; then
        [ ! -e "$STAGE" ] || exit 1
        mkdir -m 0700 "$STAGE" || exit 1
        mount -t tmpfs -o mode=0700,nosuid,nodev,size=1m tmpfs "$STAGE" || exit 1
        echo "$ID" > "$STAGE/owner"
    fi
    [ "$(cat "$STAGE/owner")" = "$ID" ] || exit 1
    make_private "$STAGE" || exit 1
    for view in /proc/self/mountinfo /proc/1/mountinfo; do
        awk -v p="$dst" '$5==p {found=1} END {exit found}' "$view" || exit 1
    done
    n=1
    while [ -e "$STAGE/bind-$$-$n" ]; do n=$((n+1)); done
    slot=$STAGE/bind-$$-$n
    mkdir -m 0700 "$slot" || exit 1
    if [ -d "$src" ]; then mkdir "$slot/entry"; elif [ -f "$src" ]; then : > "$slot/entry"; else exit 1; fi
    staged=0 published=0
    cleanup_bind() {
        rc=$?
        trap - EXIT
        if [ "$staged" = 1 ]; then umount "$slot/entry" || rc=1; fi
        if [ "$rc" -ne 0 ] && [ "$published" = 1 ]; then umount "$dst" || rc=1; fi
        exit "$rc"
    }
    trap cleanup_bind EXIT
    "$META/bind-ro" "$src" "$slot/entry" || exit 1
    staged=1
    make_private "$slot/entry" || exit 1
    mount --bind "$slot/entry" "$dst" || exit 1
    published=1
    readonly_mount_everywhere "$dst" || exit 1
    identity=$(stat -c '%d:%i' "$src")
    [ "$identity" = "$(stat -c '%d:%i' "$dst")" ] || exit 1
    [ "$identity" = "$(stat -c '%d:%i' "/proc/1/root$dst")" ] || exit 1
    echo "PUBLISHED_RO_SELF_AND_INIT target=$dst"
)

PKG=com.google.android.mosey
PERMS='ACCESS_COARSE_LOCATION ACCESS_FINE_LOCATION BLUETOOTH_SCAN BLUETOOTH_ADVERTISE NEARBY_WIFI_DEVICES ACCESS_LOCAL_NETWORK'
boot_id() { cat /proc/sys/kernel/random/boot_id; }
disable_persistently() {
    : >"$META/DISABLED"
    [ ! -d "$MOD" ] || : >"$MOD/disable"
    if [ -x "$KSUD" ]; then "$KSUD" initrc refresh || true; fi
    sync
}
check_build() {
    . "$META/profile.env"
    [ "$(getprop ro.product.device)" = "$PROFILE_DEVICE" ] || fail DEVICE_CHANGED
    [ "$(getprop ro.boot.slot_suffix)" = "$PROFILE_SLOT" ] || fail SLOT_CHANGED
    [ "$(getprop ro.vendor_dlkm.build.version.incremental)" = "$PROFILE_INCREMENTAL" ] || fail INCREMENTAL_CHANGED
    [ "$(uname -r)" = "$PROFILE_KERNEL" ] || fail KERNEL_CHANGED
    [ "$(getprop ro.build.fingerprint)" = "$(cat "$META/expected-fingerprint.txt")" ] || fail BUILD_CHANGED
    echo "$(cat "$META/expected-ksud.sha256")  $KSUD" | sha256sum -c - || fail KSUD_CHANGED
}
state_to() {
    binder_cmd env CLASSPATH="$MOD/state-classes.dex" /system/bin/app_process /system/bin IntegrationState >"$1.tmp" || return 1
    grep -qx 'STATE_COMPLETE|1' "$1.tmp" || return 1
    [ "$(grep -c '^P|' "$1.tmp")" = 6 ] || return 1
    mv "$1.tmp" "$1"
}
state_value() { awk -F'|' -v k="$2" '$1==k {print $2}' "$1"; }
permission_value() { awk -F'|' -v k="$2" -v c="$3" '$1=="P" && $2==k {print $c}' "$1"; }
radio_idle() {
    [ ! -e /dev/.houji-airdrop-mobile.lock ] && [ ! -e /dev/houji-quickshare-mobile ] &&
    [ ! -e /sys/module/wonder ] && [ ! -e /sys/module/houji_wonder_bridge ] &&
    [ ! -e /sys/class/net/mosey0 ] && [ ! -e /sys/class/net/wonder0 ] &&
    [ ! -e /sys/class/net/wondertap0 ] && [ -z "$(pidof mosey_server || true)" ]
}
