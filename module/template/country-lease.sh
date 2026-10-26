#!/system/bin/sh
# Normal Android runtime permission, leased only while the provider is in use.
# No Google flags, AppOps overrides, fake country, or permanent location grant.
set -eu
export PATH=/system/bin:/system/xbin
export LC_ALL=C
umask 077
META=/metadata/houji-qs-system79
PKG=com.google.android.mosey
PERM=android.permission.ACCESS_BACKGROUND_LOCATION
LEASE=$META/country-permission.lease
fail() { echo "COUNTRY_LEASE_ERROR=$*" >&2; exit 1; }
[ "$(id -u)" = 0 ] && [ ! -L "$META" ] && [ "$(cat "$META/owner.txt")" = houji_qs_system79 ] || fail OWNER
call() {
    if output=$(timeout -s TERM -k 1 8 "$@" </dev/null 2>&1); then rc=0; else rc=$?; fi
    [ -z "$output" ] || printf '%s\n' "$output"
    return "$rc"
}
state() {
    raw=$(call dumpsys package "$PKG") || fail STATE_READ
    line=$(printf '%s\n' "$raw" | sed -n 's/^[[:space:]]*android.permission.ACCESS_BACKGROUND_LOCATION: granted=//p' | sort -u)
    case "$line" in 'true, flags=['*']') granted=1;; 'false, flags=['*']') granted=0;; *) fail STATE_FORMAT;; esac
    [ "$(printf '%s\n' "$line" | wc -l)" = 1 ] || fail STATE_AMBIGUOUS
    flags=${line#*, flags=}
    uid=$(printf '%s\n' "$raw" | sed -n 's/^[[:space:]]*appId=\([0-9]*\)$/\1/p' | sort -u)
    case "$uid" in ''|*[!0-9]*) fail UID;; esac
    [ "$uid" -ge 10000 ] || fail UID_RANGE
    apk=$(call pm path "$PKG") || fail APK_PATH
    apk=${apk#package:}
    [ -f "$apk" ] || fail APK_PATH
    digest=$(sha256sum "$apk") || fail APK_HASH
    digest=${digest%% *}
}
restore() {
    [ -f "$LEASE" ] || { echo COUNTRY_LEASE_NONE; return 0; }
    [ ! -L "$LEASE" ] && [ "$(stat -c %u "$LEASE")" = 0 ] || fail LEDGER_OWNER
    [ "$(wc -l < "$LEASE")" = 5 ] || fail LEDGER_FORMAT
    state
    [ "$uid" = "$(sed -n '3p' "$LEASE")" ] && [ "$digest" = "$(sed -n '4p' "$LEASE")" ] || fail PACKAGE_CHANGED_PRESERVED
    [ "$flags" = "$(sed -n '5p' "$LEASE")" ] || fail PERMISSION_FLAGS_CHANGED_PRESERVED
    if [ "$granted" = 1 ]; then
        call pm revoke --user 0 "$PKG" "$PERM" || fail REVOKE
        state
        [ "$granted" = 0 ] && [ "$flags" = "$(sed -n '5p' "$LEASE")" ] || fail RESTORE_VERIFY
    fi
    mv "$LEASE" "$META/country-permission.last-restored"
    sync
    echo COUNTRY_LEASE_RESTORED
}
case "${1:-}" in
acquire)
    job=${2:-}
    printf '%s\n' "$job" | grep -Eq '^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$' || fail JOB
    [ ! -e "$LEASE" ] || fail OLD_LEASE_REQUIRES_RESTORE
    state
    [ "$digest" = "$(cat "$META/expected-apk.sha256")" ] || fail UNSUPPORTED_PROVIDER
    if [ "$granted" = 1 ]; then echo COUNTRY_PERMISSION_PREEXISTING_PRESERVED; exit 0; fi
    [ ! -e "$LEASE.tmp" ] && [ ! -L "$LEASE.tmp" ] || fail PENDING_LEDGER
    printf '%s\n' "$(cat /proc/sys/kernel/random/boot_id)" "$job" "$uid" "$digest" "$flags" > "$LEASE.tmp"
    mv "$LEASE.tmp" "$LEASE"
    sync
    # The write-ahead lease is also consumed by boot and uninstall recovery.
    call pm grant --user 0 "$PKG" "$PERM" || fail GRANT
    state
    [ "$granted" = 1 ] && [ "$flags" = "$(sed -n '5p' "$LEASE")" ] || fail GRANT_VERIFY
    echo COUNTRY_LEASE_GRANTED
    ;;
release)
    if [ -f "$LEASE" ]; then
        [ "${2:-}" = "$(sed -n '2p' "$LEASE")" ] || fail DIFFERENT_SESSION
    fi
    restore
    ;;
recover)
    [ ! -e /dev/.houji-airdrop-mobile.lock ] || fail ACTIVE_SESSION
    restore
    ;;
*) fail ACTION;;
esac
