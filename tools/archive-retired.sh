#!/system/bin/sh
# Archive only a successfully retired installation. No deletions, partition
# writes, module-state changes, radio operations or permission modifications.
set -eu
PATH=/system/bin:/system/xbin
export PATH LC_ALL=C
umask 077
META=/metadata/houji-qs-system79
MOD=/data/adb/modules/houji_qs_system79
STAGED=/data/adb/modules_update/houji_qs_system79
HOOK=/data/adb/service.d/houji-qs-system79-finalize.sh
BB=/data/adb/ksu/bin/busybox
fail() { echo "ARCHIVE_REFUSED=$*" >&2;exit 1; }
absent() { [ ! -e "$1" ] && [ ! -L "$1" ]; }
regular() { [ -f "$1" ] && [ ! -L "$1" ]; }
uuid() {
    case "$1" in ????????-????-????-????-????????????) ;; *) return 1;; esac
    case "$1" in *[!0123456789abcdef-]*) return 1;; esac
}
no_reference() {
    file=$1
    absent "$file" && return 0
    regular "$file" || fail UNKNOWN_REFERENCE_FILE
    if grep -Eq 'houji[_-]qs[_-]system79' "$file";then
        fail "LIVE_REFERENCE:$file"
    else
        grep_rc=$?
        [ "$grep_rc" = 1 ] || fail "REFERENCE_READ_FAILED:$file"
    fi
}
inactive() {
    absent "$MOD" && absent "$STAGED" || fail MODULE_STILL_PRESENT
    absent "$HOOK" || fail CLEANUP_HOOK_REMAINS
    for path in /dev/.houji-airdrop-mobile.lock /dev/houji-quickshare-mobile /dev/houji-airdrop-mobile /dev/houji-qs-auto /dev/houji-qs-system79-management-lock /sys/module/wonder /sys/module/houji_wonder_bridge /sys/class/net/mosey0 /sys/class/net/wonder0 /sys/class/net/wondertap0;do
        absent "$path" || fail "SESSION_OR_RADIO_REMAINS:$path"
    done
    # Capture ps before searching so the matching grep cannot match itself.
    processes=$(ps -A -o ARGS) || fail PROCESS_QUERY_FAILED
    if printf '%s\n' "$processes" | grep -Eq '(^|[ /])(mosey_server|RootQuickShareObserver|MobileSession|QuickShareSession|quickshare-runner\.sh|phone-runner\.sh)([[:space:]]|$)';then
        fail OWN_PROCESS_REMAINS
    else
        grep_rc=$?
        [ "$grep_rc" = 1 ] || fail PROCESS_PARSE_FAILED
    fi
    for file in /proc/self/mountinfo /proc/1/mountinfo;do
        regular "$file" || fail MOUNTINFO_MISSING
        if grep -Eq 'houji-qs-system79|houji-quickshare-mobile|/vendor/firmware_mnt/image/kiwi/amss20\.bin' "$file";then
            fail OWN_MOUNT_REMAINS
        else
            grep_rc=$?
            [ "$grep_rc" = 1 ] || fail MOUNTINFO_READ_FAILED
        fi
    done
    no_reference /metadata/watchdog/ksu/modules.rc
    no_reference /metadata/ksu/modules.rc
    if [ -d /data/adb/initrc.d ];then
        [ ! -L /data/adb/initrc.d ] || fail INITRC_DIRECTORY_IS_LINK
        for file in /data/adb/initrc.d/*.rc;do no_reference "$file";done
    fi
}

[ "$#" = 0 ] || fail NO_ARGUMENTS_SUPPORTED
[ "$(id -u)" = 0 ] || fail ROOT_REQUIRED
[ "$(getprop sys.boot_completed)" = 1 ] || fail ANDROID_NOT_READY
[ -d /metadata ] && [ ! -L /metadata ] || fail METADATA_PARENT_UNAVAILABLE
[ -d "$META" ] && [ ! -L "$META" ] && [ "$(stat -c %u "$META")" = 0 ] || fail METADATA_OWNER_OR_PATH
[ -x "$BB" ] && [ ! -L "$BB" ] || fail BUSYBOX_UNAVAILABLE
for name in owner.txt DISABLED UNINSTALL_PENDING uninstall.boot_id rollback-success.boot_id install.boot_id profile.json state-before.tsv rollback-after.tsv;do
    regular "$META/$name" || fail "MISSING_OR_UNKNOWN_RECEIPT:$name"
done
[ "$(cat "$META/owner.txt")" = houji_qs_system79 ] || fail OWNER_MISMATCH
for name in state-before.tsv rollback-after.tsv;do
    grep -qx 'STATE_COMPLETE|1' "$META/$name" || fail INCOMPLETE_PACKAGE_RECEIPT
    [ "$(grep -c '^P|' "$META/$name")" = 6 ] || fail INCOMPLETE_PERMISSION_RECEIPT
done
grep -qx 'SYSTEM_APP|0' "$META/rollback-after.tsv" && grep -qx 'DECLARED|0' "$META/rollback-after.tsv" || fail STILL_SYSTEMIZED_RECEIPT
for name in INFLIGHT failure.txt rollback-failure.txt archive-intent.txt archive-receipt.txt;do
    absent "$META/$name" || fail "FAILED_OR_UNKNOWN_STATE:$name"
done
RETIRED_BOOT=$(cat "$META/uninstall.boot_id")
SUCCESS_BOOT=$(cat "$META/rollback-success.boot_id")
INSTALL_BOOT=$(cat "$META/install.boot_id")
CURRENT_BOOT=$(cat /proc/sys/kernel/random/boot_id)
uuid "$RETIRED_BOOT" && uuid "$SUCCESS_BOOT" && uuid "$INSTALL_BOOT" && uuid "$CURRENT_BOOT" || fail INVALID_BOOT_RECEIPT
[ "$RETIRED_BOOT" = "$SUCCESS_BOOT" ] || fail ROLLBACK_BOOT_MISMATCH
[ "$INSTALL_BOOT" != "$SUCCESS_BOOT" ] || fail NO_UNINSTALL_REBOOT
[ -d "$META/permission-ledger-$SUCCESS_BOOT" ] && [ ! -L "$META/permission-ledger-$SUCCESS_BOOT" ] || fail LEDGER_NOT_CONSUMED
for entry in "$META"/permission-owned-* "$META"/permission-pending-*;do
    absent "$entry" || fail OUTSTANDING_PERMISSION_OWNERSHIP
done
TARGET=/metadata/houji-qs-system79-retired-$RETIRED_BOOT
case "$TARGET" in /metadata/houji-qs-system79-retired-????????-????-????-????-????????????) ;; *) fail UNSAFE_ARCHIVE_TARGET;; esac
absent "$TARGET" || fail ARCHIVE_TARGET_EXISTS
[ "$(stat -c %d "$META")" = "$(stat -c %d /metadata)" ] || fail DIFFERENT_FILESYSTEM
inactive
IDENTITY=$(stat -c '%d:%i' "$META")
PROFILE_SHA=$(sha256sum "$META/profile.json");PROFILE_SHA=${PROFILE_SHA%% *}
SUCCESS_SHA=$(sha256sum "$META/rollback-success.boot_id");SUCCESS_SHA=${SUCCESS_SHA%% *}
# The durable intent also serializes attempts. It is never removed, including
# on a failed rename, so an interrupted/unknown attempt needs explicit review.
(set -C; {
    echo 'FORMAT=HOUJI_RETIRED_ARCHIVE_V1'
    echo "SOURCE=$META"
    echo "TARGET=$TARGET"
    echo "SOURCE_IDENTITY=$IDENTITY"
    echo "UNINSTALL_BOOT=$RETIRED_BOOT"
    echo "ARCHIVE_BOOT=$CURRENT_BOOT"
    echo "PROFILE_SHA256=$PROFILE_SHA"
    echo "ROLLBACK_RECEIPT_SHA256=$SUCCESS_SHA"
} >"$META/archive-intent.txt") || fail INTENT_ALREADY_EXISTS
sync
inactive
absent "$TARGET" || fail ARCHIVE_TARGET_APPEARED
[ "$(stat -c '%d:%i' "$META")" = "$IDENTITY" ] || fail SOURCE_IDENTITY_CHANGED
# -T forbids treating an existing target directory as a container; -n forbids
# replacing an existing target. Both paths have the same verified parent/fs.
"$BB" mv -T -n "$META" "$TARGET" || fail RENAME_FAILED
absent "$META" || fail RENAME_DID_NOT_OCCUR
[ -d "$TARGET" ] && [ ! -L "$TARGET" ] && [ "$(stat -c '%d:%i' "$TARGET")" = "$IDENTITY" ] || fail ARCHIVE_IDENTITY_CHANGED
regular "$TARGET/archive-intent.txt" || fail ARCHIVE_INTENT_MISSING
(set -C; {
    echo 'RESULT=ARCHIVED_BY_RENAME'
    echo "SOURCE=$META"
    echo "ARCHIVE=$TARGET"
    echo "DIRECTORY_IDENTITY=$IDENTITY"
    echo "UNINSTALL_BOOT=$RETIRED_BOOT"
    echo "ARCHIVE_BOOT=$CURRENT_BOOT"
    sha256sum "$TARGET/archive-intent.txt" "$TARGET/profile.json" "$TARGET/rollback-success.boot_id" "$TARGET/state-before.tsv" "$TARGET/rollback-after.tsv"
} >"$TARGET/archive-receipt.txt") || fail RECEIPT_WRITE_FAILED
sync
echo "ARCHIVED_METADATA=$TARGET"
echo "ARCHIVE_RECEIPT=$TARGET/archive-receipt.txt"
