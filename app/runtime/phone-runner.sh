#!/system/bin/sh
# Single-device temporary sender. Never flash, force-unload or disable SELinux.
# App invokes: su -c 'sh <BASE>/phone-runner.sh <BASE> <RUN>'
# Verify same-boot or fallback reboot recovery: ... --verify-recovery <BASE> <RUN>
# Read-only readiness, with no prior session required: ... --check-ready <BASE> <RUN>
# BASE=/data/user/0/org.houji.airdrop/files/runtime
# RUN=/data/user/0/org.houji.airdrop/files/jobs/<ASCII job id>
# SHA256SUMS has exactly the FILES below (sha256sum format, relative paths).
# MobileSession and StreamingTransferProbe are in mobile-classes.dex. Identity,
# transfer.properties and source files remain private in RUN, never APK assets.
set -eu
PATH=/system/bin:/system/xbin:/vendor/bin
export PATH
BB=/data/adb/ksu/bin/busybox
APP=/data/user/0/org.houji.airdrop/files
RAM=/dev/houji-airdrop-mobile
LOCK=/dev/.houji-airdrop-mobile.lock
SERVICE=com.google.android.moseyservice.IMoseyService/default
AMSS=/vendor/firmware_mnt/image/kiwi/amss20.bin
NOTE=/sys/module/qca_cld3_kiwi_v2/notes/.note.gnu.build-id
PT=/sys/module/qca_cld3_kiwi_v2/parameters/pt_activate
PCI=/sys/bus/pci/devices/0000:01:00.0
FATAL='Kernel panic|Oops:|Unable to handle kernel|QDF BUG|Soft Unloading|FORCE_DISABLE|SSR triggered|RDDM|[Ff]irmware [Aa]ssert|Received firmware hang event|RAMDUMP DOWNLOAD MODE'
FILES='phone-runner.sh mobile-classes.dex ble-classes.dex service-probe scope-probe mosey_server local-stability.so lib64/libmosey_daemon_ffi.so modules/qca_cld3_kiwi_v2.ko modules/houji_wonder_bridge.ko modules/wonder.ko modules/wonder-probe firmware/amss20.bin'
die() {
    ERROR_REPORTED=1
    printf 'MOBILE_ERROR=%s\n' "$1"
    if [ "${UI_PIPE:-0}" = 1 ]; then printf 'MOBILE_ERROR=%s\n' "$1" >&3 || :; fi
    exit "${2:-1}"
}
require() {
    requirement=$1; shift
    if "$@"; then return 0; else die "$requirement"; fi
}
reject() {
    requirement=$1; shift
    if "$@"; then die "$requirement"; else reject_rc=$?; fi
    [ "$reject_rc" = 1 ] || die "${requirement}_READ_FAILED"
}
early_finish() {
    result=$?
    trap - EXIT
    if [ "$result" != 0 ] && [ "${ERROR_REPORTED:-0}" != 1 ]; then
        printf 'MOBILE_ERROR=ENTRY_COMMAND_FAILED\n'
    fi
    exit "$result"
}
trap early_finish EXIT
MODE=run
case "${1:-}" in --main|--server|--watchdog|--verify-recovery|--check-ready) MODE=${1#--}; shift;; esac
[ "$#" -eq 2 ] || die ARGUMENT_COUNT 64
BASE=$1
RUN=$2
require BASE_PATH test "$BASE" = "$APP/runtime"
case "$RUN" in "$APP"/jobs/*) JOB=${RUN##*/};; *) die RUN_PATH 64;; esac
case "$JOB" in ''|*[!A-Za-z0-9_-]*) die JOB_NAME 64;; esac
require RUN_PATH test "$RUN" = "$APP/jobs/$JOB"
require BASE_MISSING test -d "$BASE"
require RUN_MISSING test -d "$RUN"
# Android app mount namespaces alias /data/user/0 to /data/data. Accept only
# this exact pair for this package; init retains the /data/user/0 spelling.
BASE_REAL=$(readlink -f "$BASE") || die BASE_REALPATH_READ
RUN_REAL=$(readlink -f "$RUN") || die RUN_REALPATH_READ
case "$BASE_REAL|$RUN_REAL" in
    "$BASE|$RUN"|"/data/data/org.houji.airdrop/files/runtime|/data/data/org.houji.airdrop/files/jobs/$JOB") :;;
    *) die APP_DIRECTORY_REALPATH;;
esac
require ROOT_REQUIRED test "$(id -u)" = 0
require KSU_BUSYBOX_MISSING test -x "$BB"
APPUID=$(stat -c %u "$RUN") || die RUN_OWNER_READ
case "$APPUID" in ''|*[!0-9]*) die RUN_OWNER_INVALID 64;; esac
require RUN_OWNER_NOT_APP test "$APPUID" -ge 10000
require BASE_RUN_OWNER_MISMATCH test "$APPUID" = "$(stat -c %u "$BASE")"
umask 077

ui_line() {
    if [ "${UI_PIPE:-0}" = 1 ]; then printf '%s\n' "$1" >&3 || :; fi
}
report_error() {
    echo "MOBILE_ERROR=$1"
    ui_line "MOBILE_ERROR=$1"
}
phase() {
    printf '{"phase":"%s","partition_writes":false,"recovery_verified":false}\n' "$1" > "$RUN/status.json.tmp"
    chown "$APPUID:$APPUID" "$RUN/status.json.tmp"
    mv "$RUN/status.json.tmp" "$RUN/status.json"
    echo "MOBILE_PHASE=$1"
    ui_line "MOBILE_PHASE=$1"
}
progress_sink() {
    # Preserve complete diagnostics privately; send only this allowlist to UI.
    # Ignore broken UI pipes so a destroyed Activity cannot stop log draining.
    trap '' PIPE
    while IFS= read -r line || [ -n "$line" ]; do
        printf '%s\n' "$line" >> "$RUN/session.log"
        case "$line" in
            ask_waiting_for_user=true*) ui_line ask_waiting_for_user=true;;
            upload_http_success=true*) ui_line upload_http_success=true;;
            upload_progress_bytes=*)
                if printf '%s\n' "$line" | grep -Eq '^upload_progress_bytes=[0-9]+ total_bytes=[0-9]+$'; then ui_line "$line"; fi;;
            session_error=*) ui_line MOBILE_ERROR=SESSION_ERROR;;
            route_cleanup_error=*) ui_line MOBILE_ERROR=ROUTE_CLEANUP_FAILED;;
            mosey_stop_error=*) ui_line MOBILE_ERROR=MOSEY_STOP_FAILED;;
            MOBILE_SESSION_FAILED) ui_line MOBILE_ERROR=SESSION_FAILED;;
        esac
    done
}
cleanup_unmodified_ram() {
    [ "$RAM_CREATED" = 1 ] || return 0
    [ "$CHANGED" = 0 ] || [ "${RESTORED_DRIVERS:-0}" = 1 ] || return 1
    [ "$RAM" = /dev/houji-airdrop-mobile ] || return 1
    [ ! -L "$RAM" ] || return 1
    [ "$(readlink -f "$RAM")" = /dev/houji-airdrop-mobile ] || return 1
    [ "$(stat -c %u "$RAM")" = 0 ] || return 1
    # Reject a mounted root or child. Remove only our fixed copied assets, never
    # recursively delete or touch an existing directory from another session.
    if awk -v p="$RAM" '$5==p || index($5,p "/")==1 {found=1} END {exit !found}' /proc/self/mountinfo; then return 1; fi
    for rel in $FILES; do rm -f "$RAM/$rel" || return 1; done
    for dir in modules firmware lib64; do
        [ ! -d "$RAM/$dir" ] || rmdir "$RAM/$dir" || return 1
    done
    rmdir "$RAM"
}
owned_logs() {
    # Only direct run files produced by this session; no traversal into payloads.
    for f in "$RUN"/*.txt "$RUN"/*.log "$RUN"/*.json "$RUN"/*.pid "$RUN"/*.value "$RUN"/*.boot; do
        [ ! -f "$f" ] || chown "$APPUID:$APPUID" "$f"
    done
}
hash_is() {
    hashline=$(sha256sum "$1") || die "${3:-ASSET_HASH}_READ_FAILED"
    require "${3:-ASSET_HASH_MISMATCH}" test "${hashline%% *}" = "$2"
}
# C2_* phase/error labels are retained for the existing UI/result parser.
# They now identify the exact 20261021 stock normal-WLAN baseline below,
# not the previously flashed C2 binary. Candidate radio modules are unchanged.
original_hashes() {
    hash_is "$NOTE" a3e9bd62646444e1ecbe0f421c95ec46e6caf69a78323efb2f6ebe00e6b2bd8b C2_LOADED_HASH
    hash_is /vendor_dlkm/lib/modules/qca_cld3_kiwi_v2.ko d3fbe4f1b62f8c3533609945b617508e8a585660f5640aeeda08f8508416137b C2_DISK_HASH
    hash_is "$AMSS" 4db7b0e1aa7e4ec071708359c9d0db8d3d7872390cd8e88ffa0bbb30d560bcc8 ORIGINAL_AMSS_HASH
    hash_is /vendor/firmware_mnt/image/kiwi/phy_ucode20.elf 97c6accd597026a116a36ed2a167a1d229bd00d62653bbe381c2ec814cbbe3a7 ORIGINAL_PHY_HASH
    hash_is /vendor/firmware_mnt/image/kiwi/bd_n3.elf 866c520d8f3546af587801e2f98e9280c9eeea55fd0d8be9aca76b4790d6f0c7 ORIGINAL_BDF_HASH
    hash_is /vendor/firmware_mnt/image/kiwi/regdb_xiaomi.bin 237fc60976e5eb383ef0832e67d530d8b032e08e974fbe88a35f25c8c5784210 ORIGINAL_REGDB_HASH
}
baseline() {
    require SELINUX_NOT_ENFORCING test "$(getenforce)" = Enforcing
    require BOOT_NOT_COMPLETED test "$(getprop sys.boot_completed)" = 1
    require DEVICE_NOT_HOUJI test "$(getprop ro.product.device)" = houji
    require SLOT_NOT_B test "$(getprop ro.boot.slot_suffix)" = _b
    require KERNEL_BASELINE_MISMATCH test "$(uname -r)" = 6.1.138-android14-11-g0c3d559bcd85-ab14529422
    require ROM_BASELINE_MISMATCH test "$(getprop ro.build.fingerprint)" = 'Xiaomi/houji/houji:16/BP2A.250605.031.A3/OS3.0.307.0.WNCCNXM:user/release-keys'
    require ROM_INCREMENTAL_MISMATCH test "$(getprop ro.vendor_dlkm.build.version.incremental)" = 1792522354
    original_hashes
    require WONDER_MODULE_PRESENT test ! -e /sys/module/wonder
    require BRIDGE_MODULE_PRESENT test ! -e /sys/module/houji_wonder_bridge
    require MOSEY_INTERFACE_PRESENT test ! -e /sys/class/net/mosey0
    require WONDER_INTERFACE_PRESENT test ! -e /sys/class/net/wonder0
    require TAP_INTERFACE_PRESENT test ! -e /sys/class/net/wondertap0
    require GLOBAL_MOSEY_LIBRARY_PRESENT test ! -e /system_ext/lib64/libmosey_daemon_ffi.so
    require GLOBAL_MOSEY_SERVER_PRESENT test ! -e /system_ext/bin/mosey_server
    service_state=$(service check "$SERVICE") || die SERVICE_LOOKUP_FAILED
    printf '%s\n' "$service_state" | grep -q 'not found' || die MOSEY_SERVICE_PRESENT
    reject DEBUGFS_ALREADY_MOUNTED grep -q ' - debugfs ' /proc/1/mountinfo
    reject AMSS_ALREADY_BOUND grep -q " $AMSS " /proc/1/mountinfo
    require PCI_POWER_NOT_AUTO test "$(cat "$PCI/power/control")" = auto
    require CND_NOT_RUNNING test "$(getprop init.svc.vendor.cnd)" = running
    require WIFI_HAL_NOT_RUNNING test "$(getprop init.svc.vendor.wifi_hal_legacy)" = running
    baseline_rules=$(ip -6 rule show) || die ROUTE_RULE_READ_FAILED
    if printf '%s\n' "$baseline_rules" | grep -Eq '^10999:|lookup 10073'; then die ROUTE_RULE_COLLISION; fi
    baseline_routes=$(ip -6 route show table 10073 2>/dev/null) || baseline_routes=
    require ROUTE_TABLE_COLLISION test -z "$baseline_routes"
}
restore_settings() {
    # Both values were validated and persisted before the first change.
    for name in wifi scan; do
        val=$(cat "$RUN/original-$name.value")
        case "$val" in 0|1) :;; *) return 1;; esac
        # Persist boot-time policy without asking the disabled HAL to start on
        # the temporary firmware. The immediately following reboot applies it.
        if [ "$name" = wifi ]; then
            "$BB" timeout -s TERM -k 1 5 settings put global wifi_on "$val" || return 1
        else
            "$BB" timeout -s TERM -k 1 5 settings put global wifi_scan_always_enabled "$val" || return 1
        fi
    done
    [ "$(settings get global wifi_on)" = "$(cat "$RUN/original-wifi.value")" ] || return 1
    [ "$(settings get global wifi_scan_always_enabled)" = "$(cat "$RUN/original-scan.value")" ] || return 1
}
request_reboot() {
    # No attempt to unload an active/pinned candidate. Never clear changed marker.
    restore_settings || report_error SETTINGS_RESTORE_FAILED
    phase RECOVERY_REBOOT_REQUESTED || report_error RECOVERY_STATUS_WRITE_FAILED
    owned_logs || report_error RECOVERY_LOG_OWNER_FAILED
    sync || report_error RECOVERY_SYNC_FAILED
    /system/bin/reboot || setprop sys.powerctl reboot
}

# Transfer and recovery are separate outcomes. A successful HTTP upload does
# not prove that the receiving iPhone imported the file.
upload=false; normal=false; stop_ok=false; session_rc=null; runner_rc=null
recovery_verified=false; recovery_method=none
save_result() {
    printf '{"protocol_upload_completed":%s,"session_exit":%s,"stop_verified":%s,"normal_stop_and_no_fatal":%s,"runner_exit":%s,"actual_file_receipt_verified":false,"recovery_verified":%s,"recovery_method":"%s","partition_writes":false}\n' \
        "$upload" "$session_rc" "$stop_ok" "$normal" "$runner_rc" "$recovery_verified" "$recovery_method" > "$RUN/RUN-RESULT.json.tmp"
    chown "$APPUID:$APPUID" "$RUN/RUN-RESULT.json.tmp"
    mv "$RUN/RUN-RESULT.json.tmp" "$RUN/RUN-RESULT.json"
}
load_outcomes() {
    if [ -f "$RUN/RUN-RESULT.json" ]; then
        if grep -q '"protocol_upload_completed":true' "$RUN/RUN-RESULT.json"; then upload=true; fi
        if grep -q '"normal_stop_and_no_fatal":true' "$RUN/RUN-RESULT.json"; then normal=true; fi
        if grep -q '"stop_verified":true' "$RUN/RUN-RESULT.json"; then stop_ok=true; fi
    fi
    if [ -f "$RUN/session-exit.value" ]; then
        session_rc=$(cat "$RUN/session-exit.value")
        case "$session_rc" in ''|*[!0-9]*) die SAVED_SESSION_EXIT_INVALID;; esac
    fi
    if [ -f "$RUN/runner-exit.value" ]; then
        runner_rc=$(cat "$RUN/runner-exit.value")
        case "$runner_rc" in ''|*[!0-9]*) die SAVED_RUNNER_EXIT_INVALID;; esac
    fi
}
save_recovered_status() {
    printf '{"phase":"%s","recovery_verified":true,"recovery_method":"%s","partition_writes":false,"recovered_boot":"%s"}\n' "$recovered_phase" "$recovery_method" "$boot" > "$RUN/status.json.tmp"
    chown "$APPUID:$APPUID" "$RUN/status.json.tmp"
    mv "$RUN/status.json.tmp" "$RUN/status.json"
}
watchdog_identity() {
    case "$WATCHDOG_PID" in ''|*[!0-9]*) die WATCHDOG_PID_INVALID;; esac
    require WATCHDOG_PID_UNSAFE test "$WATCHDOG_PID" -gt 1
    if [ ! -e "/proc/$WATCHDOG_PID" ]; then return 0; fi
    wd_cmd=$(tr '\000' ' ' < "/proc/$WATCHDOG_PID/cmdline") || die WATCHDOG_CMDLINE_READ_FAILED
    case "$wd_cmd" in *"/system/bin/sh $RAM/phone-runner.sh --watchdog $BASE $RUN "*) :;; *) die WATCHDOG_PID_IDENTITY_CHANGED;; esac
    wd_stat=$(cat "/proc/$WATCHDOG_PID/stat") || die WATCHDOG_STAT_READ_FAILED
    wd_fields=${wd_stat##*) }
    set -- $wd_fields
    require WATCHDOG_GROUP_MISMATCH test "$3" = "$WATCHDOG_PID"
    require WATCHDOG_SESSION_MISMATCH test "$4" = "$WATCHDOG_PID"
    require WATCHDOG_IS_MAIN test "$WATCHDOG_PID" != "$$"
}
watchdog_group_idle() {
    # A terminated child may briefly be a zombie until its parent reaps it.
    for wd_file in /proc/[0-9]*/stat; do
        if wd_stat=$(cat "$wd_file" 2>/dev/null); then :
        elif [ ! -e "$wd_file" ]; then continue
        else return 2; fi
        wd_fields=${wd_stat##*) }
        set -- $wd_fields
        if [ "$3" = "$WATCHDOG_PID" ] && [ "$1" != Z ]; then return 1; fi
    done
    return 0
}
stop_watchdog() {
    watchdog_identity
    if [ -e "/proc/$WATCHDOG_PID" ]; then
        # Verified setsid group, including its sleep child. Killing only the
        # shell defers TERM while a foreground sleep is running on Android.
        /system/bin/kill -TERM -- "-$WATCHDOG_PID" || die WATCHDOG_GROUP_TERM_FAILED
    fi
    for wd_try in 1 2 3 4 5 6 7 8 9 10; do
        if watchdog_group_idle; then break; fi
        sleep 1
    done
    require WATCHDOG_GROUP_REMAINS watchdog_group_idle
    # All group members have exited (or are zombies); this wait cannot wait on
    # an active sleep. Reap the known child wrapper only after the bounded check.
    if [ -n "${WATCHDOG_JOB:-}" ]; then wait "$WATCHDOG_JOB" 2>/dev/null || :; fi
}

# State bits confirmed against the current ROM cnss2.ko and native-stage29.
# bit23 is historical registration state and is deliberately not required clear.
cnss_gate() {
    gate_mode=$1
    gate_bits=$2
    for gate_bit in 3 4 5 6 7 9 10 11 12 15 16 18 19 24 27; do
        require CNSS_TRANSITION_OR_RECOVERY_ACTIVE test $((gate_bits & (1 << gate_bit))) -eq 0
    done
    case "$gate_mode" in
        before)
            require CNSS_NOT_PROBED test $((gate_bits & (1 << 8))) -ne 0
            require CNSS_NOT_REGISTERED test $((gate_bits & (1 << 26))) -ne 0
            if [ $((gate_bits & (1 << 28))) -ne 0 ]; then
                require CNSS_OFF_WITH_FW_MEM_READY test $((gate_bits & (1 << 1))) -eq 0
                require CNSS_OFF_WITH_FW_READY test $((gate_bits & (1 << 2))) -eq 0
                gate_branch=powered_off
            else
                for gate_bit in 0 1 2; do require CNSS_ON_NOT_FW_READY test $((gate_bits & (1 << gate_bit))) -ne 0; done
                gate_branch=fw_ready
            fi;;
        after)
            require CNSS_UNREGISTER_NOT_POWERED_OFF test $((gate_bits & (1 << 28))) -ne 0
            for gate_bit in 1 2 8 26; do require CNSS_UNREGISTER_NOT_COMPLETE test $((gate_bits & (1 << gate_bit))) -eq 0; done
            gate_branch=unregistered_powered_off;;
        *) die CNSS_GATE_MODE_INVALID;;
    esac
    printf '%s state=%s branch=%s\n' "$gate_mode" "$gate_bits" "$gate_branch" >> "$RUN/cnss-gates.txt"
}

restore_same_boot() {
    phase NATIVE_THREADS_STOPPING
    require SESSION_PROCESS_REMAINS test ! -e "/proc/$SESSION_JOB"
    # The BLE advertiser has its own 55s bound. Wait for its actual exit before
    # checking native threads; never release a provider under a running consumer.
    wait "$BLE_JOB" || :
    require BLE_PROCESS_REMAINS test ! -e "/proc/$BLE_JOB"
    restore_rules=$(ip -6 rule show) || die RESTORE_ROUTE_RULE_READ_FAILED
    if printf '%s\n' "$restore_rules" | grep -Eq '^10999:|lookup 10073'; then die TEMPORARY_ROUTE_RULE_REMAINS; fi
    restore_routes=$(ip -6 route show table 10073 2>/dev/null) || restore_routes=
    require TEMPORARY_ROUTE_TABLE_REMAINS test -z "$restore_routes"
    server_pid=$(cat "$RUN/server.pid")
    case "$server_pid" in ''|*[!0-9]*) die SERVER_PID_INVALID;; esac
    if [ -e "/proc/$server_pid/cmdline" ]; then
        server_cmd=$(tr '\000' ' ' < "/proc/$server_pid/cmdline")
        case "$server_cmd" in *"$RAM/mosey_server"*) :;; *) die SERVER_PID_IDENTITY_CHANGED;; esac
        kill -TERM "$server_pid"
    fi
    for i in 1 2 3 4 5 6 7 8 9 10; do
        [ -e "/proc/$server_pid" ] || break
        sleep 1
    done
    require NATIVE_SERVER_STILL_RUNNING test ! -e "/proc/$server_pid"
    wait "$SERVER_JOB" || :
    service check "$SERVICE" | grep -q 'not found' || die MOSEY_BINDER_STILL_REGISTERED
    for cmdline in /proc/[0-9]*/cmdline; do
        cmdtext=$(tr '\000' ' ' < "$cmdline" 2>/dev/null) || continue
        case "$cmdtext" in *"$RAM/mosey_server"*|*"$RAM/service-probe"*|*"$RAM/scope-probe"*|*' MobileSession '*|*' StreamingTransferProbe '*|*' BleWakeProbe '*) die RADIO_PROCESS_STILL_RUNNING;; esac
    done
    require MOSEY_INTERFACE_REMAINS test ! -e /sys/class/net/mosey0
    require GLOBAL_MOSEY_LIBRARY_PRESENT test ! -e /system_ext/lib64/libmosey_daemon_ffi.so
    
    phase WONDER_UNLOADING
    "$BB" timeout -s TERM -k 2 20 rmmod wonder
    require WONDER_MODULE_REMAINS test ! -e /sys/module/wonder
    require WONDER_INTERFACE_REMAINS test ! -e /sys/class/net/wonder0
    
    phase PROVIDER_RELEASING
    # Run the sysfs write under an external timeout: a hung kernel callback must
    # trigger the independent reboot watchdog, never permit further unload steps.
    "$BB" timeout -s TERM -k 2 15 /system/bin/sh -c 'echo 1 > /sys/module/houji_wonder_bridge/parameters/pt_release'
    cat /sys/module/houji_wonder_bridge/parameters/pt_release > "$RUN/release-state.txt"
    require PROVIDER_RELEASE_NOT_CONFIRMED grep -qx 'armed=0 releasing=0 released=1 result=0' "$RUN/release-state.txt"
    cat "$PT" > "$RUN/released-pt.txt"
    require QCA_PIN_STILL_HELD grep -qx 'exclusive=1 published=0 pinned=0 sealed=1' "$RUN/released-pt.txt"
    require TAP_INTERFACE_REMAINS test ! -e /sys/class/net/wondertap0
    require QCA_REFERENCE_BUSY test "$(cat /sys/module/qca_cld3_kiwi_v2/refcnt)" = 0
    require QCA_HOLDERS_BUSY test -z "$(ls -A /sys/module/qca_cld3_kiwi_v2/holders)"
    cat /sys/kernel/debug/cnss/stats > "$RUN/cnss-released.txt"
    bits=$(sed -n 's/.*State: 0x\([0-9a-fA-F]*\).*/0x\1/p' "$RUN/cnss-released.txt")
    printf '%s\n' "$bits" | grep -Eq '^0x[0-9a-fA-F]+$' || die CNSS_RELEASED_STATE_INVALID
    # ROM cnss2.ko: bit28 is POWER_OFF, not a prerequisite for unregister.
    # Normal unregister powers down synchronously; no direct power-off request here.
    cnss_gate before "$bits"
    lsof_rc=0
    "$BB" timeout -s TERM -k 1 10 lsof > "$RUN/open-files-before-unload.txt" 2> "$RUN/open-files-errors.txt" || lsof_rc=$?
    printf '%s\n' "$lsof_rc" > "$RUN/lsof-exit.value"
    case "$lsof_rc" in 0|1) :;; *) die LSOF_FAILED_OR_TIMED_OUT;; esac
    require LSOF_OUTPUT_INCOMPLETE awk '
    NR==1 {header=($0 ~ /^[[:space:]]*COMMAND[[:space:]]+PID[[:space:]]+USER[[:space:]]+FD[[:space:]]+TYPE[[:space:]]+DEVICE[[:space:]]+SIZE\/OFF[[:space:]]+NODE[[:space:]]+NAME[[:space:]]*$/)}
    NR>1 && $2==1 {init=1}
    END {exit !(header && init)}' "$RUN/open-files-before-unload.txt"
    # Only the exact missing-proc-file race is tolerated. Never hide permission,
    # tool, timeout, malformed-output or unknown diagnostics behind rc=1.
    if [ -s "$RUN/open-files-errors.txt" ]; then
        require LSOF_UNEXPECTED_DIAGNOSTIC awk '
    /^[[:space:]]*$/ {next}
    /^lsof: \/proc\/[0-9]+\/(fd(\/[0-9]+)?|maps|stat|status|cwd|exe|root): No such file or directory$/ {races++; next}
    {bad=1}
    END {exit (bad || !races)}' "$RUN/open-files-errors.txt"
    elif [ "$lsof_rc" != 0 ]; then
        die LSOF_FAILED_WITHOUT_RACE_EVIDENCE
    fi
    reject DEBUGFS_OR_WLAN_FD_BUSY grep -Eq '/sys/kernel/debug(/|[[:space:]]|$)|/dev/wlan([[:space:]]|$)' "$RUN/open-files-before-unload.txt"
    dmesg | awk -v cut="$cutoff" '{t=$0; if (sub(/^\[ */,"",t)) {sub(/\].*/,"",t); if(t+0>=cut) print}}' > "$RUN/kernel-before-restore.txt"
    reject PRE_RESTORE_KERNEL_FATAL grep -Eq "$FATAL" "$RUN/kernel-before-restore.txt"
    
    phase CANDIDATE_UNLOADING
    "$BB" timeout -s TERM -k 2 30 rmmod qca_cld3_kiwi_v2
    require QCA_MODULE_REMAINS test ! -e /sys/module/qca_cld3_kiwi_v2
    require WLAN_DEVICE_REMAINS test ! -e /dev/wlan
    cat /sys/kernel/debug/cnss/stats > "$RUN/cnss-after-qca-unload.txt"
    bits=$(sed -n 's/.*State: 0x\([0-9a-fA-F]*\).*/0x\1/p' "$RUN/cnss-after-qca-unload.txt")
    printf '%s\n' "$bits" | grep -Eq '^0x[0-9a-fA-F]+$' || die CNSS_AFTER_UNLOAD_STATE_INVALID
    cnss_gate after "$bits"
    dmesg | awk -v cut="$cutoff" '{t=$0; if (sub(/^\[ */,"",t)) {sub(/\].*/,"",t); if(t+0>=cut) print}}' > "$RUN/kernel-after-qca-unload.txt"
    reject QCA_UNLOAD_KERNEL_FATAL grep -Eq "$FATAL" "$RUN/kernel-after-qca-unload.txt"
    require BRIDGE_REFERENCE_BUSY test "$(cat /sys/module/houji_wonder_bridge/refcnt)" = 0
    require BRIDGE_HOLDERS_BUSY test -z "$(ls -A /sys/module/houji_wonder_bridge/holders)"
    "$BB" timeout -s TERM -k 2 15 rmmod houji_wonder_bridge
    require BRIDGE_MODULE_REMAINS test ! -e /sys/module/houji_wonder_bridge
    umount "$AMSS"
    hash_is "$AMSS" 4db7b0e1aa7e4ec071708359c9d0db8d3d7872390cd8e88ffa0bbb30d560bcc8 RESTORED_AMSS_HASH
    reject AMSS_STILL_BOUND grep -q " $AMSS " /proc/1/mountinfo
    umount /sys/kernel/debug
    reject DEBUGFS_STILL_MOUNTED grep -q ' - debugfs ' /proc/1/mountinfo
    
    phase C2_RELOADING
    hash_is /vendor_dlkm/lib/modules/qca_cld3_kiwi_v2.ko d3fbe4f1b62f8c3533609945b617508e8a585660f5640aeeda08f8508416137b RESTORED_C2_DISK_HASH
    "$BB" timeout -s TERM -k 2 30 insmod /vendor_dlkm/lib/modules/qca_cld3_kiwi_v2.ko
    sleep 2
    original_hashes
    echo auto > "$PCI/power/control"
    echo houji_airdrop_mobile > /sys/power/wake_unlock
    start vendor.cnd
    start vendor.wifi_hal_legacy
    for name in wifi scan; do
        val=$(cat "$RUN/original-$name.value")
        case "$val" in 0) enabled=disabled;; 1) enabled=enabled;; *) die SAVED_WIFI_POLICY_INVALID;; esac
        if [ "$name" = wifi ]; then
            "$BB" timeout -s TERM -k 1 10 cmd wifi set-wifi-enabled "$enabled"
        else
            "$BB" timeout -s TERM -k 1 10 cmd wifi set-scan-always-available "$enabled"
        fi
    done
    for i in 1 2 3 4 5 6 7 8 9 10; do
        if [ "$(getprop init.svc.vendor.cnd)" = running ] && [ "$(getprop init.svc.vendor.wifi_hal_legacy)" = running ]; then break; fi
        sleep 1
    done
    baseline
    require WIFI_POLICY_NOT_RESTORED test "$(settings get global wifi_on)" = "$(cat "$RUN/original-wifi.value")"
    require SCAN_POLICY_NOT_RESTORED test "$(settings get global wifi_scan_always_enabled)" = "$(cat "$RUN/original-scan.value")"
    boot=$(cat /proc/sys/kernel/random/boot_id)
    require UNEXPECTED_REBOOT test "$boot" = "$(cat "$RUN/original.boot")"
    reject TEMPORARY_WAKELOCK_REMAINS grep -qw houji_airdrop_mobile /sys/power/wake_lock
    dmesg > "$RUN/kernel-after.txt"
    dmesg | awk -v cut="$cutoff" '{t=$0; if (sub(/^\[ */,"",t)) {sub(/\].*/,"",t); if(t+0>=cut) print}}' > "$RUN/kernel-restore-delta.txt"
    reject RESTORE_KERNEL_FATAL grep -Eq "$FATAL" "$RUN/kernel-restore-delta.txt"
    cmd wifi status > "$RUN/wifi-restored.txt"
    RESTORED_DRIVERS=1
    require RAM_CLEANUP_FAILED cleanup_unmodified_ram
    require RAM_REMAINS test ! -e "$RAM"
    rmdir "$LOCK"
    require LOCK_REMAINS test ! -e "$LOCK"
    # This marker commits only verified C2, settings and removal of temporary state.
    # Keep the independent guard until that commit; stop its whole verified group.
    printf '%s\n' "$boot" > "$RUN/recovered-same-boot.value.tmp"
    chown "$APPUID:$APPUID" "$RUN/recovered-same-boot.value.tmp"
    mv "$RUN/recovered-same-boot.value.tmp" "$RUN/recovered-same-boot.value"
    sync
    stop_watchdog
    recovery_verified=true
    recovery_method=same_boot
    recovered_phase=RECOVERED_C2_SAME_BOOT
    printf '%s\n' "$runner_rc" > "$RUN/runner-exit.value"
    save_result
    save_recovered_status
    owned_logs
    sync
    RESTORED=1
    # UI may allow another session only after RAM, lock and watchdog are all gone.
    echo MOBILE_PHASE=RECOVERED_C2_SAME_BOOT
    ui_line MOBILE_PHASE=RECOVERED_C2_SAME_BOOT
}

if [ "$MODE" = run ]; then
    # Firmware loader uses init's view. Only the server library mount is private.
    if "$BB" nsenter -t 1 -m -- /system/bin/sh "$BASE/phone-runner.sh" --main "$BASE" "$RUN"; then exit 0; else die NAMESPACE_OR_MAIN_FAILED "$?"; fi
fi
if [ "$MODE" = check-ready ] && [ "${MOBILE_NAMESPACE_ENTERED:-0}" != 1 ]; then
    # Exercise the same nsenter entry as run, even when already in init's view.
    # Child performs only read operations and emits readiness, not recovery.
    if MOBILE_NAMESPACE_ENTERED=1 "$BB" nsenter -t 1 -m -- /system/bin/sh "$BASE/phone-runner.sh" --check-ready "$BASE" "$RUN"; then exit 0; else die NAMESPACE_OR_READY_CHECK_FAILED "$?"; fi
fi
if [ "$MODE" = verify-recovery ] && [ "$(readlink /proc/self/ns/mnt)" != "$(readlink /proc/1/ns/mnt)" ]; then
    if "$BB" nsenter -t 1 -m -- /system/bin/sh "$BASE/phone-runner.sh" --verify-recovery "$BASE" "$RUN"; then exit 0; else die NAMESPACE_OR_RECOVERY_CHECK_FAILED "$?"; fi
fi
if [ "$MODE" = check-ready ]; then
    require INIT_NAMESPACE_REQUIRED test "$(readlink /proc/self/ns/mnt)" = "$(readlink /proc/1/ns/mnt)"
    require RAM_SESSION_PRESENT test ! -e "$RAM"
    require SESSION_LOCK_PRESENT test ! -e "$LOCK"
    baseline
    echo MOBILE_READY=true
    echo '{"phase":"READY_C2","root_verified":true,"ready":true,"recovery_verified":false,"partition_writes":false}'
    exit 0
fi
if [ "$MODE" = server ]; then
    require PRIVATE_NAMESPACE_REQUIRED test "$(readlink /proc/self/ns/mnt)" != "$(readlink /proc/1/ns/mnt)"
    "$BB" mount --make-rprivate /
    echo $$ > "$RUN/server.pid"
    readlink /proc/self/ns/mnt > "$RUN/server.namespace.txt"
    mount --bind "$RAM/lib64" /system_ext/lib64
    "$BB" mount -i -o remount,bind,ro,nosuid /system_ext/lib64
    hash_is /system_ext/lib64/libmosey_daemon_ffi.so 701ba469bcb4928889626811a7ce48456f28901a848666b88a5022c80659fa77
    export LD_PRELOAD="$RAM/local-stability.so"
    exec /system/bin/linker64 "$RAM/mosey_server"
fi
if [ "$MODE" = watchdog ]; then
    trap '' HUP
    watchdog_sleep=
    watchdog_stop() {
        trap - TERM INT EXIT
        if [ -n "$watchdog_sleep" ]; then kill -TERM "$watchdog_sleep" 2>/dev/null || :; fi
        exit 0
    }
    trap watchdog_stop TERM INT
    echo $$ > "$RUN/watchdog.pid"
    chown "$APPUID:$APPUID" "$RUN/watchdog.pid"
    echo ready > "$RUN/watchdog-ready.value"
    chown "$APPUID:$APPUID" "$RUN/watchdog-ready.value"
    # Detached session with redirected I/O; does not depend on the Java UI process.
    # Short background sleeps make TERM immediately interruptible. The main
    # runner still terminates this verified setsid group after restoration.
    watchdog_seconds=0
    while [ "$watchdog_seconds" -lt 1080 ]; do
        sleep 1 & watchdog_sleep=$!
        wait "$watchdog_sleep" || :
        watchdog_sleep=
        watchdog_seconds=$((watchdog_seconds + 1))
    done
    [ -f "$RUN/device-changed.value" ] || exit 0
    [ "$(cat /proc/sys/kernel/random/boot_id)" = "$(cat "$RUN/original.boot")" ] || exit 0
    # A committed marker means baseline/settings and RAM/lock cleanup passed.
    # Do not reboot a restored phone if main died at watchdog cancellation.
    if [ -f "$RUN/recovered-same-boot.value" ] && [ "$(cat "$RUN/recovered-same-boot.value")" = "$(cat "$RUN/original.boot")" ]; then exit 0; fi
    echo MOBILE_WATCHDOG_EXPIRED
    request_reboot
    sleep 10
    setprop sys.powerctl reboot
    die WATCHDOG_REBOOT_NOT_COMPLETED
fi
if [ "$MODE" = verify-recovery ]; then
    require RECOVERY_NO_CHANGED_MARKER test -f "$RUN/device-changed.value"
    require RECOVERY_NO_ORIGINAL_BOOT test -f "$RUN/original.boot"
    require RECOVERY_NO_ORIGINAL_WIFI test -f "$RUN/original-wifi.value"
    require RECOVERY_NO_ORIGINAL_SCAN test -f "$RUN/original-scan.value"
    boot=$(cat /proc/sys/kernel/random/boot_id)
    if [ "$boot" = "$(cat "$RUN/original.boot")" ]; then
        require RECOVERY_SAME_BOOT_NOT_COMPLETED test -f "$RUN/recovered-same-boot.value"
        require RECOVERY_SAME_BOOT_MARKER_MISMATCH test "$(cat "$RUN/recovered-same-boot.value")" = "$boot"
        require RECOVERY_WATCHDOG_RECORD_MISSING test -f "$RUN/watchdog.pid"
        WATCHDOG_PID=$(cat "$RUN/watchdog.pid")
        case "$WATCHDOG_PID" in ''|*[!0-9]*) die WATCHDOG_PID_INVALID;; esac
        require RECOVERY_WATCHDOG_REMAINS watchdog_group_idle
        recovered_phase=RECOVERED_C2_SAME_BOOT
        recovery_method=same_boot
    else
        recovered_phase=RECOVERED_C2
        recovery_method=reboot
    fi
    require RECOVERY_RAM_REMAINS test ! -e "$RAM"
    require RECOVERY_LOCK_REMAINS test ! -e "$LOCK"
    baseline
    reject RECOVERY_WAKELOCK_REMAINS grep -qw houji_airdrop_mobile /sys/power/wake_lock
    require RECOVERY_WIFI_NOT_RESTORED test "$(settings get global wifi_on)" = "$(cat "$RUN/original-wifi.value")"
    require RECOVERY_SCAN_NOT_RESTORED test "$(settings get global wifi_scan_always_enabled)" = "$(cat "$RUN/original-scan.value")"
    load_outcomes
    recovery_verified=true
    save_recovered_status
    save_result
    owned_logs
    echo "MOBILE_PHASE=$recovered_phase"
    cat "$RUN/status.json"
    exit 0
fi
require UNKNOWN_MODE test "$MODE" = main
require INIT_NAMESPACE_REQUIRED test "$(readlink /proc/self/ns/mnt)" = "$(readlink /proc/1/ns/mnt)"
require JOB_ALREADY_CHANGED_DEVICE test ! -f "$RUN/device-changed.value"
require JOB_ALREADY_STARTED test ! -f "$RUN/status.json"
mkdir -m 700 "$LOCK" || die SESSION_LOCK_PRESENT 75
CHANGED=0
RAM_CREATED=0
RESTORED_DRIVERS=0
RESTORED=0
WATCHDOG_PID=
WATCHDOG_JOB=
finish() {
    code=$?
    trap - EXIT INT TERM HUP
    if [ "$RESTORED" = 1 ]; then exit "$code"; fi
    if [ "$CHANGED" = 1 ]; then
        [ "$code" = 0 ] || report_error "RUN_EXIT_$code"
        runner_rc=$code
        [ "$runner_rc" != 0 ] || runner_rc=1
        code=$runner_rc
        printf '%s\n' "$runner_rc" > "$RUN/runner-exit.value"
        recovery_method=reboot_pending
        recovery_verified=false
        save_result || report_error RESULT_WRITE_FAILED
        if [ -f "$RUN/cancel" ]; then ui_line MOBILE_CANCELLED=true; fi
        request_reboot
    else
        report_error "PRECHECK_EXIT_$code"
        phase PRECHECK_FAILED
        if [ -n "$WATCHDOG_PID" ]; then stop_watchdog; fi
        owned_logs
        if cleanup_unmodified_ram; then rmdir "$LOCK" || :; else report_error RAM_CLEANUP_FAILED; fi
    fi
    exit "$code"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
exec 3>&1
UI_PIPE=1
exec >> "$RUN/runner.log" 2>&1
chown "$APPUID:$APPUID" "$RUN/runner.log"
phase PRECHECK
baseline
require RAM_SESSION_PRESENT test ! -e "$RAM"
require PCI_VENDOR_MISMATCH test "$(cat "$PCI/vendor")" = 0x17cb
require PCI_DEVICE_MISMATCH test "$(cat "$PCI/device")" = 0x1107
cat /proc/sys/kernel/random/boot_id > "$RUN/original.boot"
for name in wifi scan; do
    if [ "$name" = wifi ]; then val=$(settings get global wifi_on); else val=$(settings get global wifi_scan_always_enabled); fi
    case "$val" in 0|1) :;; *) die ORIGINAL_WIFI_SCAN_INVALID;; esac
    printf '%s\n' "$val" > "$RUN/original-$name.value"
done
mkdir -m 750 "$RAM"
RAM_CREATED=1
chown 0:2000 "$RAM"
mkdir -m 700 "$RAM/modules" "$RAM/firmware"
mkdir -m 755 "$RAM/lib64"
# Never source a manifest as shell code. Reject paths not in the fixed asset set.
seen=' '
while read -r digest rel extra; do
    case "$digest" in *[!0-9a-f]*|'') die MANIFEST_DIGEST_FORMAT;; esac
    require MANIFEST_DIGEST_LENGTH test "${#digest}" = 64
    require MANIFEST_EXTRA_FIELD test -z "${extra:-}"
    case " $FILES " in *" $rel "*) :;; *) die MANIFEST_UNKNOWN_PATH;; esac
    case "$seen" in *" $rel "*) die MANIFEST_DUPLICATE_PATH;; esac
    seen="$seen$rel "
    require ASSET_SYMLINK test ! -L "$BASE/$rel"
    require ASSET_MISSING test -f "$BASE/$rel"
    hash_is "$BASE/$rel" "$digest"
    cp "$BASE/$rel" "$RAM/$rel"
    hash_is "$RAM/$rel" "$digest"
done < "$BASE/SHA256SUMS"
for rel in $FILES; do case "$seen" in *" $rel "*) :;; *) die MANIFEST_MISSING_ASSET;; esac; done
hash_is "$RAM/modules/qca_cld3_kiwi_v2.ko" 5640700fb8d48b86813415149eccb0a96d66bd7e2f8c8198d22d6a32e5965a31
hash_is "$RAM/modules/houji_wonder_bridge.ko" 4acbd94943fc4c7fc400f5f1d8d51ad1820bf1243ff4532ab077227bb08b7629
hash_is "$RAM/modules/wonder.ko" 380790907bc89e6ac47eaa1ca32e269a911884cbe22a19deb6e65b6d23b7e67a
hash_is "$RAM/firmware/amss20.bin" 13f01cb46efbe498c854352bfc253fa3665c14a14c37a0a6ed6a609a48a8709c
chmod 0500 "$RAM/phone-runner.sh" "$RAM/mosey_server" "$RAM/service-probe" "$RAM/scope-probe" "$RAM/modules/wonder-probe"
chmod 0444 "$RAM/mobile-classes.dex" "$RAM/ble-classes.dex" "$RAM/lib64/libmosey_daemon_ffi.so" "$RAM/local-stability.so"
chcon u:object_r:system_lib_file:s0 "$RAM/lib64/libmosey_daemon_ffi.so" "$RAM/local-stability.so"
chown 1000:1000 "$RAM/firmware/amss20.bin"
chmod 0440 "$RAM/firmware/amss20.bin"
chcon u:object_r:firmware_file:s0 "$RAM/firmware/amss20.bin"
require IDENTITY_MISSING test -f "$RUN/identity.p12"
require TRANSFER_PROPERTIES_MISSING test -f "$RUN/transfer.properties"
/system/bin/linker64 --list "$RAM/mosey_server" > "$RUN/linker.txt"
"$BB" setsid /system/bin/sh "$RAM/phone-runner.sh" --watchdog "$BASE" "$RUN" </dev/null >> "$RUN/watchdog.log" 2>&1 &
WATCHDOG_PID=$!
WATCHDOG_JOB=$!
for i in 1 2 3 4 5; do [ ! -f "$RUN/watchdog-ready.value" ] || break; sleep 1; done
require WATCHDOG_NOT_READY test "$(cat "$RUN/watchdog-ready.value")" = ready
WATCHDOG_PID=$(cat "$RUN/watchdog.pid")
case "$WATCHDOG_PID" in ''|*[!0-9]*) die WATCHDOG_PID_INVALID;; esac
require WATCHDOG_NOT_RUNNING kill -0 "$WATCHDOG_PID"
watchdog_identity
owned_logs
CHANGED=1
echo 1 > "$RUN/device-changed.value"
chown "$APPUID:$APPUID" "$RUN/device-changed.value"
phase CONTROLS_STOPPING
stop vendor.cnd
cmd wifi set-scan-always-available disabled
cmd wifi set-wifi-enabled disabled
stop vendor.wifi_hal_legacy
mount -t debugfs -o ro,nosuid,nodev,noexec debugfs /sys/kernel/debug
sleep 3
require CND_NOT_STOPPED test "$(getprop init.svc.vendor.cnd)" = stopped
require WIFI_HAL_NOT_STOPPED test "$(getprop init.svc.vendor.wifi_hal_legacy)" = stopped
require WIFI_NOT_DISABLED test "$(settings get global wifi_on)" = 0
require WIFI_SCAN_NOT_DISABLED test "$(settings get global wifi_scan_always_enabled)" = 0
require C2_REFERENCE_BUSY test "$(cat /sys/module/qca_cld3_kiwi_v2/refcnt)" = 0
require C2_HOLDERS_BUSY test -z "$(ls -A /sys/module/qca_cld3_kiwi_v2/holders)"
cat /sys/kernel/debug/cnss/stats > "$RUN/cnss-before.txt"
bits=$(sed -n 's/.*State: 0x\([0-9a-fA-F]*\).*/0x\1/p' "$RUN/cnss-before.txt")
printf '%s\n' "$bits" | grep -Eq '^0x[0-9a-fA-F]+$' || die CNSS_STATE_INVALID
require CNSS_UNLOAD_NOT_READY test $((bits & (1 << 28))) -ne 0
for bit in 1 2 3 4 5 6 7 9 10 11 12 15 16 18 19; do require CNSS_STATE_BUSY test $((bits & (1 << bit))) -eq 0; done
require WLAN_DEVICE_MISSING test -c /dev/wlan
timeout -k 1 5 lsof /dev/wlan > "$RUN/wlan-users.txt" 2> "$RUN/wlan-users-errors.txt"
require WLAN_DEVICE_BUSY test ! -s "$RUN/wlan-users.txt"
require WLAN_USERS_CHECK_FAILED test ! -s "$RUN/wlan-users-errors.txt"
cutoff=$(cut -d ' ' -f 1 /proc/uptime)
dmesg > "$RUN/kernel-before.txt"
phase NORMAL_UNLOAD
"$BB" timeout -s TERM -k 2 30 rmmod qca_cld3_kiwi_v2
require C2_MODULE_STILL_PRESENT test ! -e /sys/module/qca_cld3_kiwi_v2
require WLAN_DEVICE_STILL_PRESENT test ! -e /dev/wlan
dmesg | awk -v cut="$cutoff" '{t=$0; if (sub(/^\[ */,"",t)) {sub(/\].*/,"",t); if(t+0>=cut) print}}' > "$RUN/unload-delta.txt"
require HARD_UNLOAD_NOT_CONFIRMED grep -q 'Hard Unloading driver' "$RUN/unload-delta.txt"
reject UNLOAD_KERNEL_FATAL grep -Eq "$FATAL" "$RUN/unload-delta.txt"
mount --bind "$RAM/firmware/amss20.bin" "$AMSS"
"$BB" mount -i -o remount,bind,ro,nosuid,relatime "$RAM/firmware/amss20.bin" "$AMSS"
hash_is "/proc/1/root$AMSS" 13f01cb46efbe498c854352bfc253fa3665c14a14c37a0a6ed6a609a48a8709c
grep " $AMSS " /proc/1/mountinfo > "$RUN/firmware-mount.txt"
require FIRMWARE_BIND_NOT_READONLY awk '{n++; if($6 !~ /(^|,)ro(,|$)/) exit 1} END {if(n!=1) exit 1}' "$RUN/firmware-mount.txt"
phase CANDIDATE_LOADING
insmod "$RAM/modules/houji_wonder_bridge.ko"
hash_is /sys/module/houji_wonder_bridge/notes/.note.gnu.build-id bf256738bdf9264c2012ef7521be77859a2c1f77676418801846355791a22272 BRIDGE_LOADED_HASH
"$BB" timeout -s TERM -k 2 30 insmod "$RAM/modules/qca_cld3_kiwi_v2.ko" pt_exclusive=1 pt_zero_serial=1 pt_rx_metadata=1
sleep 2
hash_is "$NOTE" 8a484d50e033d13c1ec17210bcfc328751318dfded4125adebc73adcec4b5692
dmesg | awk -v cut="$cutoff" '{t=$0; if (sub(/^\[ */,"",t)) {sub(/\].*/,"",t); if(t+0>=cut) print}}' > "$RUN/load-delta.txt"
require FIRMWARE_READY_NOT_CONFIRMED grep -q 'FW ready event received' "$RUN/load-delta.txt"
reject LOAD_KERNEL_FATAL grep -Eq "$FATAL" "$RUN/load-delta.txt"
require PT_EXCLUSIVE_INVALID test "$(cat /sys/module/qca_cld3_kiwi_v2/parameters/pt_exclusive)" = Y
require PT_RX_METADATA_INVALID test "$(cat /sys/module/qca_cld3_kiwi_v2/parameters/pt_rx_metadata)" = Y
require PT_ZERO_SERIAL_INVALID test "$(cat /sys/module/qca_cld3_kiwi_v2/parameters/pt_zero_serial)" = Y
require PT_LOW_COOKIE_INVALID test "$(cat /sys/module/qca_cld3_kiwi_v2/parameters/pt_low_cookie)" = N
require PT_ZERO_START_INVALID test "$(cat /sys/module/qca_cld3_kiwi_v2/parameters/pt_zero_start)" = N
echo houji_airdrop_mobile > /sys/power/wake_lock
echo on > "$PCI/power/control"
require PCI_POWER_NOT_ACTIVE test "$(cat "$PCI/power/runtime_status")" = active
# cfg80211 does not reuse wiphy indices after a same-boot unload/reload.
# Before Wonder registers, the verified QCA driver must own the only wiphy.
qca_phy_count=0
qca_phy_index=
: > "$RUN/wiphy-before-wonder.txt"
for phy_path in /sys/class/ieee80211/*; do
    [ -e "$phy_path" ] || continue
    require QCA_WIPHY_INDEX_MISSING test -f "$phy_path/index"
    phy_index=$(cat "$phy_path/index") || die QCA_WIPHY_INDEX_READ_FAILED
    printf '%s index=%s\n' "${phy_path##*/}" "$phy_index" >> "$RUN/wiphy-before-wonder.txt"
    case "$phy_index" in ''|*[!0-9]*) die QCA_WIPHY_INDEX_INVALID;; esac
    qca_phy_count=$((qca_phy_count + 1))
    qca_phy_index=$phy_index
done
require QCA_WIPHY_NOT_UNIQUE test "$qca_phy_count" = 1
printf '%s\n' "$qca_phy_index" > "$RUN/qca-wiphy-index.value"
echo 1 > "$PT"
insmod "$RAM/modules/wonder.ko" synthetic_provider=1
require WONDER_NOT_LIVE test "$(cat /sys/module/wonder/initstate)" = live
require PT_NOT_IDLE grep -q 'phase=0 error=0 closing=0 handle=0' "$PT"
if "$RAM/modules/wonder-probe" reg "$qca_phy_index" > "$RUN/country.txt" 2> "$RUN/country-errors.txt"; then :
else
    reg_rc=$?
    printf 'regulatory_query_failed wiphy=%s exit=%s\n' "$qca_phy_index" "$reg_rc"
    die REGULATORY_QUERY_FAILED "$reg_rc"
fi
require REGULATORY_COUNTRY_MISMATCH grep -q 'reg_alpha2=CN' "$RUN/country.txt"
dmesg > "$RUN/kernel-loaded.txt"
phase SESSION_STARTING
"$BB" timeout -s TERM -k 2 940 "$BB" unshare -m --propagation private /system/bin/sh "$RAM/phone-runner.sh" --server "$BASE" "$RUN" > "$RUN/server.log" 2>&1 &
SERVER_JOB=$!
for i in 1 2 3 4 5 6 7 8; do
    service check "$SERVICE" | grep -q ': found' && break
    sleep 1
done
service check "$SERVICE" | grep -q ': found' || die MOSEY_SERVER_NOT_READY
require GLOBAL_MOSEY_LIBRARY_PRESENT test ! -e /system_ext/lib64/libmosey_daemon_ffi.so
: > "$RUN/session.log"
chown "$APPUID:$APPUID" "$RUN/session.log"
mkfifo -m 600 "$RAM/progress.pipe"
progress_sink < "$RAM/progress.pipe" &
PROGRESS_JOB=$!
CLASSPATH="$RAM/mobile-classes.dex" "$BB" timeout -s TERM -k 2 930 app_process /system/bin MobileSession "$RAM" "$RUN" > "$RAM/progress.pipe" 2>&1 &
SESSION_JOB=$!
owned_logs
ready=0
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
    if grep -q MOBILE_MOSEY_READY "$RUN/session.log"; then ready=1; break; fi
    kill -0 "$SESSION_JOB" 2>/dev/null || break
    sleep 1
done
require MOSEY_START_NOT_READY test "$ready" = 1
phase DISCOVERING_AND_SENDING
"$BB" setuidgid shell /system/bin/sh -c "CLASSPATH=$RAM/ble-classes.dex timeout -s TERM -k 2 55 app_process /system/bin BleWakeProbe 45" > "$RUN/ble.log" 2>&1 &
BLE_JOB=$!
session_rc=0
wait "$SESSION_JOB" || session_rc=$?
wait "$PROGRESS_JOB" || :
rm -f "$RAM/progress.pipe"
echo "$session_rc" > "$RUN/session-exit.value"
sleep 7
cat "$PT" > "$RUN/stop-state.txt"
dmesg > "$RUN/kernel-after.txt"
dmesg | awk -v cut="$cutoff" '{t=$0; if (sub(/^\[ */,"",t)) {sub(/\].*/,"",t); if(t+0>=cut) print}}' > "$RUN/kernel-delta.txt"
stop_ok=false
if grep -qx MOBILE_STOP_OK "$RUN/session.log"; then stop_ok=true; fi
normal=false
if [ "$stop_ok" = true ] && grep -q 'phase=5 error=0' "$RUN/stop-state.txt" && ! grep -Eq "$FATAL" "$RUN/kernel-delta.txt"; then normal=true; fi
upload=false
if grep -Eq '^upload_http_success=true([[:space:]]|$)' "$RUN/session.log"; then upload=true; fi
runner_rc=$session_rc
if [ "$session_rc" = 0 ]; then
    if [ "$upload" != true ] || ! grep -qx MOBILE_SESSION_COMPLETE "$RUN/session.log"; then runner_rc=1; fi
fi
printf '%s\n' "$runner_rc" > "$RUN/runner-exit.value"
save_result
phase SESSION_FINISHED
if [ -f "$RUN/cancel" ]; then echo MOBILE_CANCELLED=true; ui_line MOBILE_CANCELLED=true; fi
# A failed/cancelled transfer can still restore without reboot after its finally
# STOP and all release prerequisites pass. Preserve its original nonzero exit.
require NORMAL_STOP_NOT_CONFIRMED test "$normal" = true
restore_same_boot
exit "$runner_rc"
