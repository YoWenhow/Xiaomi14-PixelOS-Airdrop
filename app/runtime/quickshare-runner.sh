#!/system/bin/sh
# Mobile Quick Share session; validated Stage78 driver/recovery lifecycle.
# sh BASE/quickshare-runner.sh [--check-ready|--cancel|--cancel-if-idle|--verify-recovery] BASE RUN
# BASE=/data/user/0/org.houji.airdrop/files/quickshare-runtime
# RUN=/data/user/0/org.houji.airdrop/files/jobs/<UUID>, owned by the application.
set -eu
PATH=/system/bin:/system/xbin:/vendor/bin
export PATH
BB=/data/adb/ksu/bin/busybox
APP=/data/user/0/org.houji.airdrop
APPFILES=$APP/files
RAM=/dev/houji-quickshare-mobile
LOCK=/dev/.houji-airdrop-mobile.lock
SERVICE=com.google.android.moseyservice.IMoseyService/default
AMSS=/vendor/firmware_mnt/image/kiwi/amss20.bin
NOTE=/sys/module/qca_cld3_kiwi_v2/notes/.note.gnu.build-id
PT=/sys/module/qca_cld3_kiwi_v2/parameters/pt_activate
PCI=/sys/bus/pci/devices/0000:01:00.0
FATAL='Kernel panic|Oops:|Unable to handle kernel|QDF BUG|Soft Unloading|FORCE_DISABLE|SSR triggered|RDDM|[Ff]irmware [Aa]ssert|Received firmware hang event|RAMDUMP DOWNLOAD MODE'
FILES='quickshare-runner.sh qs-classes.dex mosey_server local-stability.so lib64/libmosey_daemon_ffi.so modules/qca_cld3_kiwi_v2.ko modules/houji_wonder_bridge.ko modules/wonder.ko modules/wonder-probe firmware/amss20.bin'
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
case "${1:-}" in --main|--server|--watchdog|--verify-recovery|--check-ready|--cancel|--cancel-if-idle) MODE=${1#--}; shift;; esac
[ "$#" -eq 2 ] || die ARGUMENT_COUNT 64
BASE=$1
RUN=$2
require BASE_PATH test "$BASE" = "$APPFILES/quickshare-runtime"
case "$RUN" in "$APPFILES"/jobs/*) JOB=${RUN##*/};; *) die RUN_PATH 64;; esac
printf '%s\n' "$JOB" | grep -Eq '^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$' || die JOB_NAME 64
require RUN_PATH test "$RUN" = "$APPFILES/jobs/$JOB"
require BASE_MISSING test -d "$BASE"
require RUN_MISSING test -d "$RUN"
# Android app mount namespaces alias /data/user/0 to /data/data. Accept only
# this exact pair for this package; init retains the /data/user/0 spelling.
BASE_REAL=$(readlink -f "$BASE") || die BASE_REALPATH_READ
RUN_REAL=$(readlink -f "$RUN") || die RUN_REALPATH_READ
case "$BASE_REAL|$RUN_REAL" in
    "$BASE|$RUN"|"/data/data/org.houji.airdrop/files/quickshare-runtime|/data/data/org.houji.airdrop/files/jobs/$JOB") :;;
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
cleanup_unmodified_ram() {
    [ "$RAM_CREATED" = 1 ] || return 0
    [ "$CHANGED" = 0 ] || [ "${RESTORED_DRIVERS:-0}" = 1 ] || return 1
    [ "$RAM" = /dev/houji-quickshare-mobile ] || return 1
    [ ! -L "$RAM" ] || return 1
    [ "$(readlink -f "$RAM")" = /dev/houji-quickshare-mobile ] || return 1
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
system_module_entry_check() {
    require SYSTEM_MODULE_PENDING_REMOVE test ! -e /data/adb/modules/houji_qs_system79/remove
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
# Android shell Binder commands must not pass a protected log FD to system_server.
binder_cmd() {
    if binder_output=$("$@" </dev/null 2>&1); then binder_rc=0; else binder_rc=$?; fi
    [ -z "$binder_output" ] || printf '%s\n' "$binder_output"
    return "$binder_rc"
}
# Sysfs status deliberately uses trylock. A START/channel/STOP operation can
# return EBUSY without an error in the radio; never reuse an earlier snapshot.
pt_snapshot() {
    pt_dest=$1
    pt_tmp=$pt_dest.next
    pt_err=$pt_dest.read-error
    [ ! -L "$pt_tmp" ] && [ ! -L "$pt_err" ] || die PT_SNAPSHOT_SYMLINK
    pt_now=$(uptime_centiseconds) || die PT_SNAPSHOT_CLOCK_FAILED
    pt_until=$((pt_now+500))
    pt_tries=0
    while :; do
        if LC_ALL=C "$BB" timeout -s TERM -k 1 1 cat "$PT" > "$pt_tmp" 2> "$pt_err"; then
            [ -s "$pt_tmp" ] || die PT_SNAPSHOT_EMPTY
            mv -f "$pt_tmp" "$pt_dest" || die PT_SNAPSHOT_COMMIT_FAILED
            if [ "$pt_tries" -gt 0 ]; then
                printf 'snapshot=%s busy_retries=%s\n' "${pt_dest##*/}" "$pt_tries" >> "$RUN/pt-read-retries.log"
            fi
            return 0
        fi
        # Only this exact single errno diagnostic is retryable. Timeouts,
        # permission errors, missing nodes and mixed diagnostics remain fatal.
        awk -v wanted="cat: $PT: Device or resource busy" '
            {n++; if ($0 != wanted) bad=1}
            END {exit !(n==1 && !bad)}' "$pt_err" || die PT_SNAPSHOT_READ_FAILED
        pt_now=$(uptime_centiseconds) || die PT_SNAPSHOT_CLOCK_FAILED
        [ "$pt_now" -lt "$pt_until" ] || die PT_SNAPSHOT_BUSY_TIMEOUT
        pt_tries=$((pt_tries+1))
        "$BB" sleep 0.1
    done
}

# dmesg uses kernel log time, which need not include suspended time like
# /proc/uptime does. Establish and compare the boundary in the same domain.
kernel_snapshot_cutoff() {
    awk '
        match($0, /^\[[[:space:]]*[0-9]+\.[0-9]+\]/) {
            last=substr($0, 2, RLENGTH-2)
            sub(/^[[:space:]]*/, "", last)
            found=1
        }
        END {if (!found) exit 1; print last}' "$1"
}
kernel_delta() {
    awk -v cut="$1" '
        match($0, /^\[[[:space:]]*[0-9]+\.[0-9]+\]/) {
            timestamp=substr($0, 2, RLENGTH-2)
            sub(/^[[:space:]]*/, "", timestamp)
            if (timestamp+0 > cut+0) print
        }'
}

provider_quiet() {
    binder_cmd "$BB" timeout -s TERM -k 1 8 am force-stop com.google.android.mosey || return 1
    binder_cmd "$BB" timeout -s TERM -k 1 8 pm disable --user 0 com.google.android.mosey/.ExternalSharingService || return 1
    [ -z "$(pidof com.google.android.mosey 2>/dev/null)" ] || return 1
    provider_temp_remove || return 1
    country_permission_release
}
country_permission_release() {
    binder_cmd "$BB" timeout -s TERM -k 2 30 /system/bin/sh /data/adb/modules/houji_qs_system79/country-lease.sh release "$JOB" > "$RUN/country-permission-restored.txt"
}
# Session-scoped exemption only. It expires in 120s and is renewed every 45s;
# no permanent whitelist or global idle policy is modified.
provider_lease_renew() {
    lease_now=$1
    if binder_cmd "$BB" timeout -s TERM -k 1 8 cmd deviceidle tempwhitelist -u 0 -d 120000 com.google.android.mosey > "$RUN/provider-whitelist.txt"; then
        PROVIDER_RENEW_AT=$((lease_now+45))
        printf 'uptime=%s renewed=1 next=%s\n' "$lease_now" "$PROVIDER_RENEW_AT" >> "$RUN/provider-renewals.log"
        return 0
    fi
    printf 'uptime=%s renewed=0\n' "$lease_now" >> "$RUN/provider-renewals.log"
    return 1
}
provider_temp_remove() {
    binder_cmd "$BB" timeout -s TERM -k 1 8 cmd deviceidle tempwhitelist -r -u 0 com.google.android.mosey > "$RUN/provider-whitelist-removed.txt"
}
provider_stop_evidence() {
    awk -v pid="$GOOGLE_PID" '
    $3==pid && $6=="Mosey.A" {
        message=substr($0,index($0,"Mosey.A")+length("Mosey.A"))
        sub(/^[[:space:]]*:[[:space:]]*/,"",message)
        sub(/\r$/,"",message)
        if (message=="ExternalSharingService doDestroy called." ||
            message=="Mosey invoking Shutdown" ||
            message=="API: shutdown() called.") stopped=1
    }
    END {exit !stopped}' "$1"
}

provider_precheck() {
    hash_is "$BASE/qs-classes.dex" 8a2e83a467dfc570db9f5dde8316e000995bacca523f367d9674028c68cf88cf QS_DEX_HASH
    probe_text=$(CLASSPATH="$BASE/qs-classes.dex" "$BB" timeout -s TERM -k 1 12 app_process /system/bin IntegrationProbe) || die GOOGLE_PROBE_FAILED
    printf '%s\n' "$probe_text" > "$RUN/integration-preflight.txt"
    if ! printf '%s\n' "$probe_text" | grep -Fq '"systemApp":true'; then die GOOGLE_NOT_SYSTEM_APP; fi
    if ! printf '%s\n' "$probe_text" | grep -Fq '"declared":true'; then die GOOGLE_HAL_NOT_DECLARED; fi
    if ! printf '%s\n' "$probe_text" | grep -Fq '"componentEnabledSetting":2'; then die GOOGLE_COMPONENT_NOT_DISABLED; fi
    runtime_grants_pending=0
    for perm in BLUETOOTH_SCAN BLUETOOTH_ADVERTISE ACCESS_FINE_LOCATION ACCESS_LOCAL_NETWORK CREATE_APP_SPECIFIC_NETWORK MANAGE_WIFI_INTERFACES MANAGE_WIFI_NETWORK_SELECTION; do
        if printf '%s\n' "$probe_text" | grep -q "\"$perm\":true"; then continue; fi
        case "$MODE:$perm" in
            check-ready:BLUETOOTH_SCAN|check-ready:BLUETOOTH_ADVERTISE|check-ready:ACCESS_FINE_LOCATION|check-ready:ACCESS_LOCAL_NETWORK)
                printf '%s\n' "$probe_text" | grep -q "\"$perm\":false" || die GOOGLE_RUNTIME_PERMISSION_STATE_MISSING
                runtime_grants_pending=1;;
            *) die "GOOGLE_PERMISSION_MISSING_$perm";;
        esac
    done
    if [ "$runtime_grants_pending" = 1 ]; then echo MOBILE_RUNTIME_GRANTS_PENDING=true; fi
}
pt_is_idle() {
    grep -Eq '(^| )phase=0 error=0 closing=0 handle=0( |$)' "$1"
}
pt_is_drained() {
    grep -Eq '(^| )phase=5 error=0( |$)' "$1"
}
pt_is_active() {
    grep -Eq '(^| )phase=[1-4] error=0 closing=[01] handle=1( |$)' "$1"
}

# Framework Wi-Fi is only an administrative STA interface in this candidate.
# Never issue connect, scan, interface-up or native START from the controller.
qca_admin_snapshot() {
    snap_file=$1
    QCA_NETDEV_COUNT=0 QCA_UP_COUNT=0 QCA_CLIENT_UP=0
    snap_index=$(cat "$RUN/qca-wiphy-index.value") || die QCA_INDEX_READ_FAILED
    case "$snap_index" in ''|*[!0-9]*) die QCA_INDEX_INVALID;; esac
    snap_phys=0
    for snap_phy in /sys/class/ieee80211/*; do
        [ -f "$snap_phy/index" ] || continue
        if [ "$(cat "$snap_phy/index")" = "$snap_index" ]; then snap_phys=$((snap_phys+1)); fi
    done
    require QCA_WIPHY_DISAPPEARED test "$snap_phys" = 1
    : > "$snap_file"
    for snap_dev in /sys/class/net/*; do
        [ -f "$snap_dev/phy80211/index" ] || continue
        snap_owner=$(cat "$snap_dev/phy80211/index" 2>/dev/null) || continue
        [ "$snap_owner" = "$snap_index" ] || continue
        snap_flags=$(cat "$snap_dev/flags" 2>/dev/null) || {
            [ ! -e "$snap_dev" ] && continue
            die QCA_NETDEV_FLAGS_READ_FAILED
        }
        printf '%s\n' "$snap_flags" | grep -Eq '^0x[0-9a-fA-F]+$' || die QCA_NETDEV_FLAGS_INVALID
        snap_up=$((snap_flags & 1))
        QCA_NETDEV_COUNT=$((QCA_NETDEV_COUNT+1))
        QCA_UP_COUNT=$((QCA_UP_COUNT+snap_up))
        snap_name=${snap_dev##*/}
        if [ "$snap_name" = "${FRAMEWORK_IFACE:-}" ] && [ "$snap_up" = 1 ]; then QCA_CLIENT_UP=1; fi
        printf 'iface=%s wiphy=%s flags=%s up=%s\n' "$snap_name" "$snap_index" "$snap_flags" "$snap_up" >> "$snap_file"
    done
    printf 'interfaces=%s up=%s\n' "$QCA_NETDEV_COUNT" "$QCA_UP_COUNT" >> "$snap_file"
}
# Entry shutdown must follow framework completion before stopping its HAL.
# Each command is bounded; snapshots are retained even when admission fails.
controls_stop_command() {
    controls_command_name=$1; shift
    if binder_cmd "$BB" timeout -s TERM -k 1 8 "$@" > "$RUN/controls-command-$controls_command_name.txt" 2>&1; then
        printf 'command=%s rc=0\n' "$controls_command_name" >> "$RUN/controls-stop-history.txt"
    else
        controls_command_rc=$?
        printf 'command=%s rc=%s\n' "$controls_command_name" "$controls_command_rc" >> "$RUN/controls-stop-history.txt"
        die CONTROLS_STOP_COMMAND_FAILED
    fi
}
controls_stop_read() {
    controls_read_name=$1; shift
    if binder_cmd "$BB" timeout -s TERM -k 1 3 "$@" > "$RUN/controls-$controls_read_name-$CTRL_SAMPLE.txt" 2>&1; then
        :
    else
        controls_read_rc=$?
        printf 'sample=%s phase=%s read=%s rc=%s\n' "$CTRL_SAMPLE" "$CTRL_STAGE" "$controls_read_name" "$controls_read_rc" >> "$RUN/controls-stop-history.txt"
        die CONTROLS_STOP_STATE_READ_FAILED
    fi
}
controls_stop_snapshot() {
    CTRL_SAMPLE=$((CTRL_SAMPLE+1))
    controls_stop_read framework cmd wifi status
    controls_stop_read wifi-on settings get global wifi_on
    controls_stop_read scan settings get global wifi_scan_always_enabled
    controls_stop_read cnd getprop init.svc.vendor.cnd
    controls_stop_read hal getprop init.svc.vendor.wifi_hal_legacy
    CTRL_FRAMEWORK=$(sed -n -e 's/^Wifi is enabled$/enabled/p' -e 's/^Wifi is disabled$/disabled/p' "$RUN/controls-framework-$CTRL_SAMPLE.txt")
    CTRL_WIFI=$(cat "$RUN/controls-wifi-on-$CTRL_SAMPLE.txt")
    CTRL_SCAN=$(cat "$RUN/controls-scan-$CTRL_SAMPLE.txt")
    CTRL_CND=$(cat "$RUN/controls-cnd-$CTRL_SAMPLE.txt")
    CTRL_HAL=$(cat "$RUN/controls-hal-$CTRL_SAMPLE.txt")
    CTRL_NOW=$(uptime_seconds) || die CONTROLS_STOP_CLOCK_FAILED
    printf 'sample=%s phase=%s uptime=%s framework=%s wifi_on=%s scan=%s cnd=%s hal=%s\n' "$CTRL_SAMPLE" "$CTRL_STAGE" "$CTRL_NOW" "$CTRL_FRAMEWORK" "$CTRL_WIFI" "$CTRL_SCAN" "$CTRL_CND" "$CTRL_HAL" >> "$RUN/controls-stop-history.txt"
    case "$CTRL_FRAMEWORK" in enabled|disabled) :;; *) die CONTROLS_FRAMEWORK_STATE_UNKNOWN;; esac
    case "$CTRL_WIFI" in 0|1) :;; *) die CONTROLS_WIFI_POLICY_UNKNOWN;; esac
    case "$CTRL_SCAN" in 0|1) :;; *) die CONTROLS_SCAN_POLICY_UNKNOWN;; esac
    case "$CTRL_CND" in running|stopping|restarting|stopped) :;; *) die CONTROLS_CND_STATE_UNKNOWN;; esac
    case "$CTRL_HAL" in running|stopping|restarting|stopped) :;; *) die CONTROLS_HAL_STATE_UNKNOWN;; esac
}
controls_framework_is_disabled() {
    [ "$CTRL_FRAMEWORK" = disabled ] && [ "$CTRL_WIFI" = 0 ] && [ "$CTRL_SCAN" = 0 ]
}
controls_stop_synchronized() {
    : > "$RUN/controls-stop-history.txt"
    CTRL_SAMPLE=0 CTRL_STAGE=framework CTRL_STABLE=0
    controls_stop_command cnd-stop stop vendor.cnd
    controls_stop_command scan-disable cmd wifi set-scan-always-available disabled
    controls_stop_command wifi-disable svc wifi disable
    CTRL_NOW=$(uptime_seconds) || die CONTROLS_STOP_CLOCK_FAILED
    CTRL_DEADLINE=$((CTRL_NOW+30))
    CTRL_TRIES=0
    while [ "$CTRL_TRIES" -lt 30 ]; do
        CTRL_TRIES=$((CTRL_TRIES+1))
        controls_stop_snapshot
        [ "$CTRL_NOW" -le "$CTRL_DEADLINE" ] || break
        if controls_framework_is_disabled; then CTRL_STABLE=$((CTRL_STABLE+1)); else CTRL_STABLE=0; fi
        [ "$CTRL_STABLE" -lt 2 ] || break
        [ "$CTRL_NOW" -lt "$CTRL_DEADLINE" ] || break
        sleep 1
    done
    require CONTROLS_FRAMEWORK_DISABLE_TIMEOUT test "$CTRL_STABLE" -ge 2
    # Do not kill the HAL while framework teardown can request it again.
    CTRL_STAGE=hal CTRL_STABLE=0 CTRL_TRIES=0 CTRL_HAL_STOP_COUNT=1
    controls_stop_command hal-stop-1 stop vendor.wifi_hal_legacy
    CTRL_NOW=$(uptime_seconds) || die CONTROLS_STOP_CLOCK_FAILED
    CTRL_DEADLINE=$((CTRL_NOW+15))
    while [ "$CTRL_TRIES" -lt 15 ]; do
        CTRL_TRIES=$((CTRL_TRIES+1))
        controls_stop_snapshot
        [ "$CTRL_NOW" -le "$CTRL_DEADLINE" ] || break
        # A fresh enable after confirmed disable is a competing request, not
        # permission to force the framework down or unload a live driver.
        require CONTROLS_FRAMEWORK_REENABLED controls_framework_is_disabled
        if [ "$CTRL_CND" = stopped ] && [ "$CTRL_HAL" = stopped ]; then
            CTRL_STABLE=$((CTRL_STABLE+1))
        else
            CTRL_STABLE=0
            case "$CTRL_HAL" in
                running|restarting)
                    if [ "$CTRL_HAL_STOP_COUNT" -lt 3 ]; then
                        CTRL_HAL_STOP_COUNT=$((CTRL_HAL_STOP_COUNT+1))
                        controls_stop_command "hal-stop-$CTRL_HAL_STOP_COUNT" stop vendor.wifi_hal_legacy
                    fi;;
            esac
        fi
        [ "$CTRL_STABLE" -lt 2 ] || break
        [ "$CTRL_NOW" -lt "$CTRL_DEADLINE" ] || break
        sleep 1
    done
    require CONTROLS_HAL_STOP_TIMEOUT test "$CTRL_STABLE" -ge 2
    echo CONTROLS_STOPPED_TWO_CONFIRMED_SNAPSHOTS
}

framework_wifi_start() {
    phase FRAMEWORK_WIFI_STARTING
    require SAVED_HAL_NOT_RUNNING test "$(cat "$RUN/original-hal.value")" = running
    start vendor.wifi_hal_legacy
    binder_cmd "$BB" timeout -s TERM -k 1 8 svc wifi enable > "$RUN/framework-enable.txt"
    framework_ready=0
    for framework_try in 1 2 3 4 5 6 7 8 9 10; do
        binder_cmd "$BB" timeout -s TERM -k 1 3 cmd wifi status > "$RUN/framework-status.txt"
        FRAMEWORK_IFACE=$(sed -n 's/.*iface=\([A-Za-z0-9_.-]*\) role=ROLE_CLIENT_PRIMARY.*/\1/p' "$RUN/framework-status.txt")
        case "$FRAMEWORK_IFACE" in ''|*[!A-Za-z0-9_.-]*) FRAMEWORK_IFACE=;; esac
        qca_admin_snapshot "$RUN/framework-qca-interfaces.txt"
        if grep -qx 'Wifi is enabled' "$RUN/framework-status.txt" &&
            [ "$(getprop init.svc.vendor.wifi_hal_legacy)" = running ] &&
            [ "$QCA_CLIENT_UP" = 1 ]; then framework_ready=1; break; fi
        sleep 1
    done
    require FRAMEWORK_WIFI_NOT_READY test "$framework_ready" = 1
    require FRAMEWORK_SCAN_NOT_DISABLED test "$(settings get global wifi_scan_always_enabled)" = 0
    pt_snapshot "$RUN/pt-before-google.txt"
    require PT_STARTED_BEFORE_GOOGLE pt_is_idle "$RUN/pt-before-google.txt"
    # Diagnostic evidence only; runtime admission is enforced by the candidate.
    dmesg | kernel_delta "$cutoff" | grep -F 'PT administrative STA up, no firmware vdev' > "$RUN/framework-admin-up.txt" || :
    echo FRAMEWORK_WIFI_READY_ADMIN_ONLY
}
framework_wifi_stop() {
    phase FRAMEWORK_WIFI_STOPPING
    binder_cmd "$BB" timeout -s TERM -k 1 8 svc wifi disable > "$RUN/framework-disable.txt"
    framework_disabled=0
    for framework_try in 1 2 3 4 5 6 7 8 9 10; do
        binder_cmd "$BB" timeout -s TERM -k 1 3 cmd wifi status > "$RUN/framework-disabled-status.txt"
        if grep -qx 'Wifi is disabled' "$RUN/framework-disabled-status.txt"; then framework_disabled=1; break; fi
        sleep 1
    done
    require FRAMEWORK_WIFI_NOT_DISABLED test "$framework_disabled" = 1
    stop vendor.wifi_hal_legacy
    framework_down=0
    for framework_try in 1 2 3 4 5 6 7 8 9 10; do
        qca_admin_snapshot "$RUN/framework-qca-down.txt"
        if [ "$(getprop init.svc.vendor.wifi_hal_legacy)" = stopped ] && [ "$QCA_UP_COUNT" = 0 ]; then framework_down=1; break; fi
        sleep 1
    done
    require FRAMEWORK_HAL_OR_QCA_STILL_UP test "$framework_down" = 1
    require FRAMEWORK_WIFI_POLICY_NOT_DISABLED test "$(settings get global wifi_on)" = 0
    require FRAMEWORK_SCAN_NOT_DISABLED test "$(settings get global wifi_scan_always_enabled)" = 0
    echo FRAMEWORK_HAL_STOPPED_ALL_QCA_INTERFACES_DOWN
}

cancel_requested() {
    require CANCEL_SYMLINK test ! -L "$RUN/cancel"
    [ -e "$RUN/cancel" ] || return 1
    require CANCEL_NOT_FILE test -f "$RUN/cancel"
    require CANCEL_NOT_ROOT_OWNED test "$(stat -c %u "$RUN/cancel")" = 0
    require CANCEL_JOB_MISMATCH test "$(cat "$RUN/cancel")" = "$JOB"
}
release_session_lock() {
    [ ! -L "$LOCK" ] && [ "$(stat -c %u "$LOCK")" = 0 ] || return 1
    [ ! -L "$LOCK/quickshare-job" ] && [ "$(cat "$LOCK/quickshare-job")" = "$RUN" ] || return 1
    rm "$LOCK/quickshare-job" || return 1
    rmdir "$LOCK"
}
uptime_seconds() {
    read -r clock_uptime clock_rest < /proc/uptime
    clock_seconds=${clock_uptime%%.*}
    case "$clock_seconds" in ''|*[!0-9]*) return 1;; esac
    printf '%s\n' "$clock_seconds"
}
save_outcome() {
    printf '%s\n' "$SESSION_OUTCOME" > "$RUN/session-outcome.value"
    printf '%s\n' "$SESSION_RC" > "$RUN/session-exit.value"
    printf '%s\n' "$TRANSFER_DIRECTION" > "$RUN/transfer-direction.value"
    printf '%s\n' "$TRANSFER_COMPLETED" > "$RUN/transfer-completed.value"
}
load_outcome() {
    TIMEOUT_REASON=$(cat "$RUN/session-timeout.value" 2>/dev/null) || TIMEOUT_REASON=none
    case "$TIMEOUT_REASON" in none|idle_limit|active_hard_limit) :;; *) die SAVED_TIMEOUT_REASON_INVALID;; esac
    SESSION_OUTCOME=$(cat "$RUN/session-outcome.value" 2>/dev/null) || SESSION_OUTCOME=interrupted
    SESSION_RC=$(cat "$RUN/session-exit.value" 2>/dev/null) || SESSION_RC=1
    TRANSFER_DIRECTION=$(cat "$RUN/transfer-direction.value" 2>/dev/null) || TRANSFER_DIRECTION=unknown
    TRANSFER_COMPLETED=$(cat "$RUN/transfer-completed.value" 2>/dev/null) || TRANSFER_COMPLETED=false
    case "$SESSION_OUTCOME" in completed|cancelled|timed_out|native_stopped|log_unavailable|provider_stopped|provider_refresh_failed|interrupted) :;; *) die SAVED_OUTCOME_INVALID;; esac
    case "$SESSION_RC" in 0|1|124|130) :;; *) die SAVED_SESSION_EXIT_INVALID;; esac
    case "$TRANSFER_DIRECTION" in send|receive|unknown) :;; *) die SAVED_DIRECTION_INVALID;; esac
    case "$TRANSFER_COMPLETED" in true|false) :;; *) die SAVED_TRANSFER_RESULT_INVALID;; esac
}
save_result() {
    printf '{"phase":"%s","session_outcome":"%s","timeout_reason":"%s","protocol_transfer_completed":%s,"transfer_direction":"%s","actual_file_receipt_verified":false,"session_exit":%s,"runner_exit":%s,"radio_exit_branch":"%s","recovery_verified":%s,"recovery_method":"%s","partition_writes":false}\n' \
        "$RESULT_PHASE" "$SESSION_OUTCOME" "${TIMEOUT_REASON:-none}" "$TRANSFER_COMPLETED" "$TRANSFER_DIRECTION" "$SESSION_RC" "$SESSION_RC" "${RADIO_EXIT:-UNKNOWN}" "$RECOVERY_VERIFIED" "$RECOVERY_METHOD" > "$RUN/RUN-RESULT.json.tmp"
    chown "$APPUID:$APPUID" "$RUN/RUN-RESULT.json.tmp"
    mv "$RUN/RUN-RESULT.json.tmp" "$RUN/RUN-RESULT.json"
}
provider_disabled_check() {
    require GOOGLE_PROCESS_REMAINS test -z "$(pidof com.google.android.mosey 2>/dev/null)"
    disabled_probe=$(CLASSPATH="$BASE/qs-classes.dex" "$BB" timeout -s TERM -k 1 12 app_process /system/bin IntegrationProbe) || die GOOGLE_DISABLED_CHECK_FAILED
    printf '%s\n' "$disabled_probe" | grep -Fq '"componentEnabledSetting":2' || die GOOGLE_COMPONENT_NOT_DISABLED
}

# Parse only the newly created Google process, never another job's END_TRANSFER.
# Success without a matching START/END, or while another transfer is active,
# cannot close the radio. Unlabelled success is accepted only with one transfer.
google_outcome() {
    awk -v pid="$GOOGLE_PID" '
    function id(line, x) {
        x=line; sub(/^.*transferId=/,"",x); sub(/[,)].*$/,"",x)
        if (length(x)!=36 || x !~ /^[0-9a-fA-F-]+$/) return ""
        return tolower(x)
    }
    $3==pid && $6=="Mosey.A" {
        message=substr($0,index($0,"Mosey.A")+length("Mosey.A"))
        sub(/^[[:space:]]*:[[:space:]]*/,"",message)
        if (index(message,"Event received: START_TRANSFER(transferId=")==1) {
            x=id(message)
            if (x!="" && !active[x]) {active[x]=1; count++; current=x}
            if (count>1) ambiguous=1
        } else if (index(message,"Receiving upload for DvzipTransferId(transferId=")==1) {
            x=id(message); if (active[x]) received[x]=1
        } else if (message=="UPLOAD request succeeded." || message=="UPLOAD request succeeded.\r") {
            if (count==1) sent[current]=1
        } else if (index(message,"Media scanner successfully returned URI: content://")==1) {
            if (count==1 && received[current]) saved[current]=1
        } else if (index(message,"Event received: END_TRANSFER(transferId=")==1) {
            x=id(message)
            if (x!="" && active[x]) {
                active[x]=0; count--; ended++
                if (sent[x]) {completed++; direction="send"}
                else if (received[x] && saved[x]) {completed++; direction="receive"}
            }
        }
    }
    END {
        if (!ambiguous && count==0 && completed>0) print "completed=" direction
        else print "pending"
    }' "$1"
}

# Count every current-PID START, including a transfer that finishes between
# samples: its final callback must still receive a fresh settling interval.
google_transfer_generation() {
    awk -v pid="$GOOGLE_PID" '
    $3==pid && $6=="Mosey.A" {
        message=substr($0,index($0,"Mosey.A")+length("Mosey.A"))
        sub(/^[[:space:]]*:[[:space:]]*/,"",message)
        if (index(message,"Event received: START_TRANSFER(transferId=")==1) n++
    }
    END {print n+0}' "$1"
}
uptime_centiseconds() {
    awk '$1 ~ /^[0-9]+\.[0-9][0-9]$/ {split($1,t,"[.]"); printf "%.0f\n", t[1]*100+t[2]; ok=1} END {if (!ok) exit 1}' /proc/uptime
}
settle_update() {
    SETTLE_READY=0
    SETTLE_RESTARTED=0
    case "$1" in
        completed=send|completed=receive)
            COMPLETION_SEEN=1
            if [ "$SETTLE_DUE" = 0 ] || [ "$SETTLE_GENERATION" != "$2" ]; then
                SETTLE_DUE=$(($3+800))
                SETTLE_GENERATION=$2
                SETTLE_RESTARTED=1
            elif [ "$3" -ge "$SETTLE_DUE" ]; then
                SETTLE_READY=1
            fi;;
        *) SETTLE_DUE=0;;
    esac
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

# Only Google file-transfer START extends waiting time. Native START is merely
# radio discovery and must not turn an unused share window into a 30-minute job.
transfer_timeout_update() {
    TIMEOUT_EXPIRED=0
    ACTIVE_TIMEOUT_STARTED=0
    if [ "$ACTIVE_DEADLINE" = 0 ] && [ "$2" -gt 0 ]; then
        ACTIVE_DEADLINE=$(($1+1800))
        [ "$ACTIVE_DEADLINE" -le "$SESSION_HARD_DEADLINE" ] || ACTIVE_DEADLINE=$SESSION_HARD_DEADLINE
        ACTIVE_TIMEOUT_STARTED=1
    fi
    if [ "$ACTIVE_DEADLINE" != 0 ]; then
        if [ "$1" -ge "$ACTIVE_DEADLINE" ]; then
            # A completed transfer may finish its existing eight-second callback
            # grace. Repeated STARTs cannot move the fixed hard cap beyond +8s.
            if [ "$SETTLE_DUE" -gt 0 ] && [ "$SETTLE_DUE" -le "$(((ACTIVE_DEADLINE+8)*100))" ]; then return 0; fi
            TIMEOUT_REASON=active_hard_limit
            TIMEOUT_EXPIRED=1
        fi
    elif [ "$1" -ge "$IDLE_DEADLINE" ]; then
        TIMEOUT_REASON=idle_limit
        TIMEOUT_EXPIRED=1
    fi
}
retain_transfer_evidence() {
    # Ring-buffer snapshots may lose START during a large upload. Preserve only
    # the few current-PID protocol rows needed for pairing, once each, per job.
    awk -v pid="$GOOGLE_PID" '
    FILENAME==ARGV[1] {seen[$0]=1; next}
    $3==pid && $6=="Mosey.A" {
        message=substr($0,index($0,"Mosey.A")+length("Mosey.A"))
        sub(/^[[:space:]]*:[[:space:]]*/,"",message)
        if ((index(message,"Event received: START_TRANSFER(transferId=")==1 ||
             index(message,"Event received: END_TRANSFER(transferId=")==1 ||
             index(message,"Receiving upload for DvzipTransferId(transferId=")==1 ||
             index(message,"UPLOAD request succeeded.")==1 ||
             index(message,"Media scanner successfully returned URI: content://")==1 ||
             message=="ExternalSharingService doDestroy called." ||
             message=="Mosey invoking Shutdown" ||
             message=="API: shutdown() called.") && !seen[$0]++) print
    }' "$RUN/transfer-evidence.log" "$1" > "$RUN/new-transfer-evidence.tmp"
    cat "$RUN/new-transfer-evidence.tmp" >> "$RUN/transfer-evidence.log"
    rm "$RUN/new-transfer-evidence.tmp"
}
idle_cancel_requested() {
    require IDLE_CANCEL_SYMLINK test ! -L "$RUN/idle-cancel"
    [ -e "$RUN/idle-cancel" ] || return 1
    require IDLE_CANCEL_NOT_FILE test -f "$RUN/idle-cancel"
    require IDLE_CANCEL_NOT_ROOT_OWNED test "$(stat -c %u "$RUN/idle-cancel")" = 0
    require IDLE_CANCEL_JOB_MISMATCH test "$(cat "$RUN/idle-cancel")" = "$JOB"
}

# Read only a root-owned, atomic marker from this exact job and boot. Invalid
# markers never delay fallback. The marker's name is excluded from owned_logs.
watchdog_extension_deadline() {
    case "$watchdog_start" in ''|*[!0-9]*) return 1;; esac
    case "$watchdog_now" in ''|*[!0-9]*) return 1;; esac
    [ ! -L "$RUN/transfer-start" ] && [ -f "$RUN/transfer-start" ] || return 1
    [ "$(stat -c %u "$RUN/transfer-start")" = 0 ] || return 1
    [ "$(wc -l < "$RUN/transfer-start")" = 1 ] || return 1
    read -r wd_job wd_boot wd_transfer wd_extra < "$RUN/transfer-start" || return 1
    [ "$wd_job" = "$JOB" ] && [ "$wd_boot" = "$(cat /proc/sys/kernel/random/boot_id)" ] && [ -z "$wd_extra" ] || return 1
    case "$wd_transfer" in ''|*[!0-9]*) return 1;; esac
    [ "$wd_transfer" -ge "$watchdog_start" ] && [ "$wd_transfer" -le "$watchdog_now" ] || return 1
    WD_EXTEND_UNTIL=$((wd_transfer+1800+240))
    wd_hard_cap=$((watchdog_start+2400))
    [ "$WD_EXTEND_UNTIL" -le "$wd_hard_cap" ] || WD_EXTEND_UNTIL=$wd_hard_cap
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
    provider_quiet || report_error GOOGLE_PROVIDER_QUIET_FAILED
    restore_settings || report_error SETTINGS_RESTORE_FAILED
    phase RECOVERY_REBOOT_REQUESTED || report_error RECOVERY_STATUS_WRITE_FAILED
    owned_logs || report_error RECOVERY_LOG_OWNER_FAILED
    sync || report_error RECOVERY_SYNC_FAILED
    /system/bin/reboot || setprop sys.powerctl reboot
}

# Before the first rmmod attempt the original firmware/driver are untouched.
# A competing framework enable is then a cancelled admission, not a reason to
# reboot. This runs in a subshell: any failed assertion returns to the existing
# guarded reboot path. Never use this after even an attempted driver unload.
recover_controls_only() (
    require CONTROLS_RECOVERY_DRIVER_TOUCHED test "$DRIVER_TOUCHED" = 0
    require CONTROLS_RECOVERY_BOOT_CHANGED test "$(cat /proc/sys/kernel/random/boot_id)" = "$(cat "$RUN/original.boot")"
    original_hashes
    require CONTROLS_RECOVERY_WONDER_PRESENT test ! -e /sys/module/wonder
    require CONTROLS_RECOVERY_BRIDGE_PRESENT test ! -e /sys/module/houji_wonder_bridge
    reject CONTROLS_RECOVERY_AMSS_BOUND grep -q " $AMSS " /proc/1/mountinfo
    phase CONTROLS_RESTORING
    if [ "$DEBUGFS_OWNED" = 1 ]; then
        require CONTROLS_DEBUGFS_UNMOUNT_FAILED umount /sys/kernel/debug
    fi
    require CONTROLS_COUNTRY_RESTORE_FAILED country_permission_release
    require CONTROLS_CND_START_FAILED start vendor.cnd
    require CONTROLS_HAL_START_FAILED start vendor.wifi_hal_legacy
    for name in wifi scan; do
        val=$(cat "$RUN/original-$name.value") || die CONTROLS_SAVED_POLICY_READ
        case "$val" in 0) enabled=disabled;; 1) enabled=enabled;; *) die CONTROLS_SAVED_POLICY_INVALID;; esac
        if [ "$name" = wifi ]; then
            require CONTROLS_WIFI_RESTORE_FAILED binder_cmd "$BB" timeout -s TERM -k 1 10 cmd wifi set-wifi-enabled "$enabled"
        else
            require CONTROLS_SCAN_RESTORE_FAILED binder_cmd "$BB" timeout -s TERM -k 1 10 cmd wifi set-scan-always-available "$enabled"
        fi
    done
    for i in 1 2 3 4 5 6 7 8 9 10; do
        [ "$(getprop init.svc.vendor.cnd)" != running ] || [ "$(getprop init.svc.vendor.wifi_hal_legacy)" != running ] || break
        sleep 1
    done
    baseline
    require CONTROLS_WIFI_POLICY_NOT_RESTORED test "$(settings get global wifi_on)" = "$(cat "$RUN/original-wifi.value")"
    require CONTROLS_SCAN_POLICY_NOT_RESTORED test "$(settings get global wifi_scan_always_enabled)" = "$(cat "$RUN/original-scan.value")"
    require CONTROLS_WIFI_STATUS_READ binder_cmd "$BB" timeout -s TERM -k 1 5 cmd wifi status > "$RUN/wifi-restored.txt"
    RESTORED_DRIVERS=1
    require CONTROLS_RAM_CLEANUP_FAILED cleanup_unmodified_ram
    boot=$(cat /proc/sys/kernel/random/boot_id) || die CONTROLS_BOOT_READ
    require CONTROLS_BOOT_CHANGED test "$boot" = "$(cat "$RUN/original.boot")"
    printf '%s\n' "$boot" > "$RUN/recovered-same-boot.value"
    chown "$APPUID:$APPUID" "$RUN/recovered-same-boot.value"
    sync
    kill -TERM "$WATCHDOG_PID" 2>/dev/null || :
    for i in 1 2 3 4 5; do
        if watchdog_group_idle; then break; fi
        sleep 1
    done
    require CONTROLS_WATCHDOG_REMAINS watchdog_group_idle
    require CONTROLS_LOCK_CLEANUP_FAILED release_session_lock
    load_outcome
    RESULT_PHASE=RECOVERED_C2_SAME_BOOT RECOVERY_VERIFIED=true RECOVERY_METHOD=controls_only_same_boot RADIO_EXIT=NOT_UNLOADED
    save_result
    cp "$RUN/RUN-RESULT.json" "$RUN/status.json"
    owned_logs
    sync
    echo MOBILE_PHASE=RECOVERED_C2_SAME_BOOT
    ui_line MOBILE_PHASE=RECOVERED_C2_SAME_BOOT
)

if [ "$MODE" = run ]; then
    # Firmware loader uses init's view. Only the server library mount is private.
    if "$BB" nsenter -t 1 -m -- /system/bin/sh "$BASE/quickshare-runner.sh" --main "$BASE" "$RUN"; then result=0; else result=$?; fi
    trap - EXIT
    exit "$result"
fi
if { [ "$MODE" = cancel ] || [ "$MODE" = cancel-if-idle ] || [ "$MODE" = verify-recovery ]; } && [ "$(readlink /proc/self/ns/mnt)" != "$(readlink /proc/1/ns/mnt)" ]; then
    if "$BB" nsenter -t 1 -m -- /system/bin/sh "$BASE/quickshare-runner.sh" "--$MODE" "$BASE" "$RUN"; then exit 0; else exit "$?"; fi
fi
if [ "$MODE" = cancel ] || [ "$MODE" = cancel-if-idle ]; then
    cancel_marker=cancel
    [ "$MODE" != cancel-if-idle ] || cancel_marker=idle-cancel
    require CANCEL_NO_ACTIVE_SESSION test -d "$LOCK"
    require CANCEL_LOCK_NOT_ROOT test "$(stat -c %u "$LOCK")" = 0
    require CANCEL_DIFFERENT_JOB test "$(cat "$LOCK/quickshare-job")" = "$RUN"
    # The shared lock lives in /dev and cannot survive a reboot. Allow cancel
    # during preflight, before original.boot has been persisted in RUN.
    require CANCEL_SYMLINK test ! -L "$RUN/$cancel_marker"
    require CANCEL_TEMP_SYMLINK test ! -L "$RUN/$cancel_marker.tmp"
    require CANCEL_TEMP_PRESENT test ! -e "$RUN/$cancel_marker.tmp"
    printf '%s\n' "$JOB" > "$RUN/$cancel_marker.tmp"
    chmod 0600 "$RUN/$cancel_marker.tmp"
    chown 0:0 "$RUN/$cancel_marker.tmp"
    mv "$RUN/$cancel_marker.tmp" "$RUN/$cancel_marker"
    if [ "$MODE" = cancel-if-idle ]; then echo MOBILE_IDLE_CANCEL_REQUESTED=true; else echo MOBILE_CANCEL_REQUESTED=true; fi
    exit 0
fi
if [ "$MODE" = verify-recovery ]; then
    require RECOVERY_NO_CHANGED_MARKER test -f "$RUN/device-changed.value"
    boot=$(cat /proc/sys/kernel/random/boot_id)
    if [ "$boot" = "$(cat "$RUN/original.boot")" ]; then
        require RECOVERY_SAME_BOOT_NOT_COMPLETED test "$(cat "$RUN/recovered-same-boot.value")" = "$boot"
        WATCHDOG_PID=$(cat "$RUN/watchdog.pid")
        case "$WATCHDOG_PID" in ''|*[!0-9]*) die WATCHDOG_PID_INVALID;; esac
        require RECOVERY_WATCHDOG_REMAINS watchdog_group_idle
        RESULT_PHASE=RECOVERED_C2_SAME_BOOT
        RECOVERY_METHOD=same_boot
    else
        RESULT_PHASE=RECOVERED_C2
        RECOVERY_METHOD=reboot
    fi
    require RECOVERY_RAM_REMAINS test ! -e "$RAM"
    require RECOVERY_LOCK_REMAINS test ! -e "$LOCK"
    baseline
    provider_disabled_check
    reject RECOVERY_WAKELOCK_REMAINS grep -qw houji_quickshare_mobile /sys/power/wake_lock
    require RECOVERY_WIFI_NOT_RESTORED test "$(settings get global wifi_on)" = "$(cat "$RUN/original-wifi.value")"
    require RECOVERY_SCAN_NOT_RESTORED test "$(settings get global wifi_scan_always_enabled)" = "$(cat "$RUN/original-scan.value")"
    load_outcome
    RADIO_EXIT=$(cat "$RUN/radio-exit-branch.value" 2>/dev/null) || RADIO_EXIT=UNKNOWN
    case "$RADIO_EXIT" in IDLE|DRAINED|UNKNOWN) :;; *) die SAVED_RADIO_EXIT_INVALID;; esac
    RECOVERY_VERIFIED=true
    save_result
    cp "$RUN/RUN-RESULT.json" "$RUN/status.json"
    owned_logs
    echo "MOBILE_PHASE=$RESULT_PHASE"
    cat "$RUN/status.json"
    exit 0
fi
if [ "$MODE" = check-ready ] && [ "${MOBILE_NAMESPACE_ENTERED:-0}" != 1 ]; then
    # Exercise the same nsenter entry as run, even when already in init's view.
    # Child performs only read operations and emits readiness, not recovery.
    if MOBILE_NAMESPACE_ENTERED=1 "$BB" nsenter -t 1 -m -- /system/bin/sh "$BASE/quickshare-runner.sh" --check-ready "$BASE" "$RUN"; then exit 0; else die NAMESPACE_OR_READY_CHECK_FAILED "$?"; fi
fi
if [ "$MODE" = check-ready ]; then
    system_module_entry_check
    require INIT_NAMESPACE_REQUIRED test "$(readlink /proc/self/ns/mnt)" = "$(readlink /proc/1/ns/mnt)"
    require RAM_SESSION_PRESENT test ! -e "$RAM"
    require SESSION_LOCK_PRESENT test ! -e "$LOCK"
    baseline
    provider_precheck
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
    echo $$ > "$RUN/watchdog.pid"
    chown "$APPUID:$APPUID" "$RUN/watchdog.pid"
    echo ready > "$RUN/watchdog-ready.value"
    chown "$APPUID:$APPUID" "$RUN/watchdog-ready.value"
    # Detached session with redirected I/O; does not depend on the Java UI process.
    # The reboot itself ends this process. No boot service or restart registration.
    # Keep the timer as a background child: TERM interrupts wait immediately.
    # A foreground sleep delays mksh's trap until the full waiting interval expire.
    timer_pid=
    cancel_watchdog() {
        trap - EXIT INT TERM HUP
        [ -z "$timer_pid" ] || kill -TERM "$timer_pid" 2>/dev/null || :
        [ -z "$timer_pid" ] || wait "$timer_pid" 2>/dev/null || :
        exit 0
    }
    trap cancel_watchdog INT TERM
    watchdog_start=$(uptime_seconds) || watchdog_start=
    sleep 480 &
    timer_pid=$!
    wait "$timer_pid" || exit 0
    timer_pid=
    if [ "$(cat "$RUN/recovered-same-boot.value" 2>/dev/null)" = "$(cat "$RUN/original.boot")" ]; then exit 0; fi
    [ -f "$RUN/device-changed.value" ] || exit 0
    [ "$(cat /proc/sys/kernel/random/boot_id)" = "$(cat "$RUN/original.boot")" ] || exit 0
    watchdog_now=$(uptime_seconds) || watchdog_now=
    if watchdog_extension_deadline; then
        remaining=$((WD_EXTEND_UNTIL-watchdog_now))
        if [ "$remaining" -gt 0 ]; then
            echo "MOBILE_WATCHDOG_EXTENDED_UNTIL=$WD_EXTEND_UNTIL"
            sleep "$remaining" &
            timer_pid=$!
            wait "$timer_pid" || exit 0
            timer_pid=
        fi
    else
        echo MOBILE_WATCHDOG_NO_VALID_TRANSFER_EXTENSION
    fi
    if [ "$(cat "$RUN/recovered-same-boot.value" 2>/dev/null)" = "$(cat "$RUN/original.boot")" ]; then exit 0; fi
    [ -f "$RUN/device-changed.value" ] || exit 0
    [ "$(cat /proc/sys/kernel/random/boot_id)" = "$(cat "$RUN/original.boot")" ] || exit 0
    echo MOBILE_WATCHDOG_EXPIRED
    request_reboot
    sleep 10
    setprop sys.powerctl reboot
    die WATCHDOG_REBOOT_NOT_COMPLETED
fi
require UNKNOWN_MODE test "$MODE" = main
# Reject only new sessions. Cancel, recovery, server and watchdog remain usable.
system_module_entry_check
require INIT_NAMESPACE_REQUIRED test "$(readlink /proc/self/ns/mnt)" = "$(readlink /proc/1/ns/mnt)"
require JOB_ALREADY_CHANGED_DEVICE test ! -f "$RUN/device-changed.value"
require JOB_ALREADY_STARTED test ! -f "$RUN/status.json"
require TRANSFER_MARKER_PRESENT test ! -e "$RUN/transfer-start"
require TRANSFER_MARKER_SYMLINK test ! -L "$RUN/transfer-start"
require TRANSFER_MARKER_TEMP_PRESENT test ! -e "$RUN/transfer-start.tmp"
require TRANSFER_MARKER_TEMP_SYMLINK test ! -L "$RUN/transfer-start.tmp"
mkdir -m 700 "$LOCK" || die SESSION_LOCK_PRESENT 75
printf '%s\n' "$RUN" > "$LOCK/quickshare-job"
CHANGED=0
DRIVER_TOUCHED=0
DEBUGFS_OWNED=0
RESTORED=0
RESTORED_DRIVERS=0
RAM_CREATED=0
WATCHDOG_PID=
finish() {
    code=$?
    trap - EXIT INT TERM HUP
    if [ "$RESTORED" = 1 ]; then
        exit "$code"
    elif [ "$CHANGED" = 1 ]; then
        [ "$code" = 0 ] || report_error "RUN_EXIT_$code"
        if [ "$DRIVER_TOUCHED" = 0 ] && recover_controls_only; then
            wait "$WATCHDOG_PID" 2>/dev/null || :
            exit "$code"
        fi
        if [ ! -f "$RUN/RUN-RESULT.json" ]; then
            printf '{"protocol_upload_completed":false,"actual_file_receipt_verified":false,"recovery_verified":false,"exit_code":%s,"partition_writes":false}\n' "$code" > "$RUN/RUN-RESULT.json"
        fi
        request_reboot
    else
        report_error "PRECHECK_EXIT_$code"
        country_permission_release || report_error COUNTRY_PRECHECK_RESTORE_FAILED
        phase PRECHECK_FAILED
        [ -z "$WATCHDOG_PID" ] || kill "$WATCHDOG_PID" 2>/dev/null || :
        owned_logs
        if cleanup_unmodified_ram; then release_session_lock || report_error LOCK_CLEANUP_FAILED; else report_error RAM_CLEANUP_FAILED; fi
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
require SYSTEM_MODULE_MISSING test -f /data/adb/modules/houji_qs_system79/manage.sh
binder_cmd "$BB" timeout -s TERM -k 2 20 /system/bin/sh /data/adb/modules/houji_qs_system79/manage.sh grant-runtime > "$RUN/runtime-permissions.txt"
baseline
provider_precheck
require GOOGLE_PROCESS_PRESENT test -z "$(pidof com.google.android.mosey 2>/dev/null)"
require COUNTRY_LEASE_HELPER_MISSING test -f /data/adb/modules/houji_qs_system79/country-lease.sh
require COUNTRY_PERMISSION_FAILED binder_cmd "$BB" timeout -s TERM -k 2 30 /system/bin/sh /data/adb/modules/houji_qs_system79/country-lease.sh acquire "$JOB" > "$RUN/country-permission.txt"
require RAM_SESSION_PRESENT test ! -e "$RAM"
require PCI_VENDOR_MISMATCH test "$(cat "$PCI/vendor")" = 0x17cb
require PCI_DEVICE_MISMATCH test "$(cat "$PCI/device")" = 0x1107
cat /proc/sys/kernel/random/boot_id > "$RUN/original.boot"
getprop init.svc.vendor.wifi_hal_legacy > "$RUN/original-hal.value"
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
hash_is "$RAM/modules/qca_cld3_kiwi_v2.ko" 5359ad849814ba64867f0ee6c974f80c07085a9c9019d70baafe8a26af7a9fc0
hash_is "$RAM/modules/houji_wonder_bridge.ko" 4acbd94943fc4c7fc400f5f1d8d51ad1820bf1243ff4532ab077227bb08b7629
hash_is "$RAM/modules/wonder.ko" 380790907bc89e6ac47eaa1ca32e269a911884cbe22a19deb6e65b6d23b7e67a
hash_is "$RAM/firmware/amss20.bin" 13f01cb46efbe498c854352bfc253fa3665c14a14c37a0a6ed6a609a48a8709c
chmod 0500 "$RAM/quickshare-runner.sh" "$RAM/mosey_server" "$RAM/modules/wonder-probe"
chmod 0444 "$RAM/qs-classes.dex" "$RAM/lib64/libmosey_daemon_ffi.so" "$RAM/local-stability.so"
chcon u:object_r:system_lib_file:s0 "$RAM/lib64/libmosey_daemon_ffi.so" "$RAM/local-stability.so"
chown 1000:1000 "$RAM/firmware/amss20.bin"
chmod 0440 "$RAM/firmware/amss20.bin"
chcon u:object_r:firmware_file:s0 "$RAM/firmware/amss20.bin"
/system/bin/linker64 --list "$RAM/mosey_server" > "$RUN/linker.txt"
"$BB" setsid /system/bin/sh "$RAM/quickshare-runner.sh" --watchdog "$BASE" "$RUN" </dev/null >> "$RUN/watchdog.log" 2>&1 &
WATCHDOG_PID=$!
for i in 1 2 3 4 5; do [ ! -f "$RUN/watchdog-ready.value" ] || break; sleep 1; done
require WATCHDOG_NOT_READY test "$(cat "$RUN/watchdog-ready.value")" = ready
WATCHDOG_PID=$(cat "$RUN/watchdog.pid")
case "$WATCHDOG_PID" in ''|*[!0-9]*) die WATCHDOG_PID_INVALID;; esac
require WATCHDOG_NOT_RUNNING kill -0 "$WATCHDOG_PID"
owned_logs
CHANGED=1
echo 1 > "$RUN/device-changed.value"
chown "$APPUID:$APPUID" "$RUN/device-changed.value"
phase CONTROLS_STOPPING
controls_stop_synchronized
mount -t debugfs -o ro,nosuid,nodev,noexec debugfs /sys/kernel/debug
DEBUGFS_OWNED=1
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
dmesg > "$RUN/kernel-before.txt"
cutoff=$(kernel_snapshot_cutoff "$RUN/kernel-before.txt") || die KERNEL_BASELINE_TIMESTAMP_INVALID
printf '%s\n' "$cutoff" > "$RUN/kernel-cutoff.value"
phase NORMAL_UNLOAD
# Last admission snapshot closes the long preflight window. Never rmmod after
# a newly enabled framework, even if the earlier two snapshots were clean.
CTRL_STAGE=unload-admission
controls_stop_snapshot
require CONTROLS_FRAMEWORK_REENABLED controls_framework_is_disabled
require CONTROLS_SERVICES_RESTARTED test "$CTRL_CND:$CTRL_HAL" = stopped:stopped
DRIVER_TOUCHED=1
echo 1 > "$RUN/driver-unload-attempted.value"
"$BB" timeout -s TERM -k 2 30 rmmod qca_cld3_kiwi_v2
require C2_MODULE_STILL_PRESENT test ! -e /sys/module/qca_cld3_kiwi_v2
require WLAN_DEVICE_STILL_PRESENT test ! -e /dev/wlan
dmesg | kernel_delta "$cutoff" > "$RUN/unload-delta.txt"
require HARD_UNLOAD_NOT_CONFIRMED grep -q 'Hard Unloading driver' "$RUN/unload-delta.txt"
reject UNLOAD_KERNEL_FATAL grep -Eq "$FATAL" "$RUN/unload-delta.txt"
mount --bind "$RAM/firmware/amss20.bin" "$AMSS"
"$BB" mount -i -o remount,bind,ro,nosuid,relatime "$RAM/firmware/amss20.bin" "$AMSS"
hash_is "/proc/1/root$AMSS" 13f01cb46efbe498c854352bfc253fa3665c14a14c37a0a6ed6a609a48a8709c
grep " $AMSS " /proc/1/mountinfo > "$RUN/firmware-mount.txt"
require FIRMWARE_BIND_NOT_READONLY awk '{n++; if($6 !~ /(^|,)ro(,|$)/) exit 1} END {if(n!=1) exit 1}' "$RUN/firmware-mount.txt"
phase CANDIDATE_LOADING
insmod "$RAM/modules/houji_wonder_bridge.ko"
hash_is /sys/module/houji_wonder_bridge/notes/.note.gnu.build-id bf256738bdf9264c2012ef7521be77859a2c1f77676418801846355791a22272 BRIDGE_LOADED_NOTE
require RELEASE_PARAMETER_MISSING test -f /sys/module/houji_wonder_bridge/parameters/pt_release
"$BB" timeout -s TERM -k 2 30 insmod "$RAM/modules/qca_cld3_kiwi_v2.ko" pt_exclusive=1 pt_zero_serial=1 pt_rx_metadata=1 pt_framework_netdev=1
sleep 2
hash_is "$NOTE" 66ec5eb01ac67fb4b197f1cba71439849b4edcb4b9661b3d3b14eb4c6f864d2a
dmesg | kernel_delta "$cutoff" > "$RUN/load-delta.txt"
require FIRMWARE_READY_NOT_CONFIRMED grep -q 'FW ready event received' "$RUN/load-delta.txt"
reject LOAD_KERNEL_FATAL grep -Eq "$FATAL" "$RUN/load-delta.txt"
require PT_EXCLUSIVE_INVALID test "$(cat /sys/module/qca_cld3_kiwi_v2/parameters/pt_exclusive)" = Y
require PT_ADMIN_UP_INVALID test "$(cat /sys/module/qca_cld3_kiwi_v2/parameters/pt_framework_netdev)" = Y
require PT_RX_METADATA_INVALID test "$(cat /sys/module/qca_cld3_kiwi_v2/parameters/pt_rx_metadata)" = Y
require PT_ZERO_SERIAL_INVALID test "$(cat /sys/module/qca_cld3_kiwi_v2/parameters/pt_zero_serial)" = Y
require PT_LOW_COOKIE_INVALID test "$(cat /sys/module/qca_cld3_kiwi_v2/parameters/pt_low_cookie)" = N
require PT_ZERO_START_INVALID test "$(cat /sys/module/qca_cld3_kiwi_v2/parameters/pt_zero_start)" = N
echo houji_quickshare_mobile > /sys/power/wake_lock
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

# Google owns BLE, native START lifetime, NetworkAgent, discovery and transfers.
phase QUICKSHARE_SESSION
"$BB" timeout -s TERM -k 2 2160 "$BB" unshare -m --propagation private /system/bin/sh "$RAM/quickshare-runner.sh" --server "$BASE" "$RUN" > "$RUN/server.log" 2>&1 &
SERVER_JOB=$!
for i in 1 2 3 4 5 6 7 8; do
    if service check "$SERVICE" | grep -q ': found'; then break; fi
    sleep 1
done
service check "$SERVICE" | grep -q ': found' || die MOSEY_SERVER_NOT_READY
require GLOBAL_MOSEY_LIBRARY_PRESENT test ! -e /system_ext/lib64/libmosey_daemon_ffi.so
framework_wifi_start
provider_initial_now=$(uptime_seconds) || die PROVIDER_CLOCK_READ_FAILED
# Acquire the exemption before enabling or starting the provider. If that
# fails, PT is still idle and the existing normal restore path remains safe.
if provider_lease_renew "$provider_initial_now"; then
GOOGLE_LOG_SINCE=$(date '+%m-%d %H:%M:%S.000')
binder_cmd "$BB" timeout -s TERM -k 1 8 pm default-state --user 0 com.google.android.mosey/.ExternalSharingService > "$RUN/provider-enable.txt"
binder_cmd "$BB" timeout -s TERM -k 1 10 am startservice --include-stopped-packages -n com.google.android.mosey/.ExternalSharingService > "$RUN/provider-start.txt"
reject GOOGLE_PROVIDER_START_ERROR grep -Eq 'Error:|Exception|Failed transaction' "$RUN/provider-start.txt"
require GOOGLE_PROVIDER_START_NOT_CONFIRMED grep -q '^Starting service:' "$RUN/provider-start.txt"
GOOGLE_PID=$(pidof com.google.android.mosey) || die GOOGLE_PROCESS_NOT_READY
case "$GOOGLE_PID" in ''|*[!0-9]*) die GOOGLE_PID_NOT_UNIQUE;; esac
printf '%s\n' "$GOOGLE_PID" > "$RUN/google.pid"
# A cancellation made during setup must not reopen the Google UI.
if ! cancel_requested && ! idle_cancel_requested; then
    phase QUICKSHARE_READY
    echo QUICKSHARE_READY=1
    ui_line QUICKSHARE_READY=1
fi
START_SEEN=0
SESSION_OUTCOME=timed_out
SESSION_RC=124
TRANSFER_COMPLETED=false
TRANSFER_DIRECTION=unknown
COMPLETION_SEEN=0
SETTLE_DUE=0
SETTLE_GENERATION=0
observation=0
IDLE_DEADLINE=$(uptime_seconds) || die SESSION_CLOCK_READ_FAILED
IDLE_DEADLINE=$((IDLE_DEADLINE+180))
SESSION_HARD_DEADLINE=$((IDLE_DEADLINE+1800))
ACTIVE_DEADLINE=0
TIMEOUT_REASON=none
: > "$RUN/transfer-evidence.log"
while :; do
    pt_snapshot "$RUN/pt-current.txt"
    printf 'sample=%s ' "$observation" >> "$RUN/pt-observe.txt"
    cat "$RUN/pt-current.txt" >> "$RUN/pt-observe.txt"
    require PT_OBSERVE_ERROR grep -Eq '(^| )error=0( |$)' "$RUN/pt-current.txt"
    if ! pt_is_idle "$RUN/pt-current.txt"; then START_SEEN=1; fi
    if cancel_requested; then
        SESSION_OUTCOME=cancelled
        SESSION_RC=130
        echo MOBILE_CANCELLED=true
        ui_line MOBILE_CANCELLED=true
        break
    fi
    if "$BB" timeout -s TERM -k 1 3 logcat -d -v threadtime -T "$GOOGLE_LOG_SINCE" --pid="$GOOGLE_PID" Mosey.A:I '*:S' > "$RUN/quickshare-logcat.txt" 2> "$RUN/quickshare-logcat-errors.txt"; then
        retain_transfer_evidence "$RUN/quickshare-logcat.txt"
        outcome=$(google_outcome "$RUN/transfer-evidence.log")
        generation=$(google_transfer_generation "$RUN/transfer-evidence.log")
        settle_now=$(uptime_centiseconds) || die SETTLING_CLOCK_READ_FAILED
        settle_update "$outcome" "$generation" "$settle_now"
        # Read current START evidence before the idle deadline decision: a file
        # beginning at the last sample must not be cut off as idle discovery.
        now=$((settle_now/100))
        transfer_timeout_update "$now" "$generation"
        if [ "$ACTIVE_TIMEOUT_STARTED" = 1 ]; then
            # This exact job's first real Google file START alone extends guard.
            printf '%s %s %s\n' "$JOB" "$(cat /proc/sys/kernel/random/boot_id)" "$now" > "$RUN/transfer-start.tmp"
            chown 0:0 "$RUN/transfer-start.tmp"
            chmod 0400 "$RUN/transfer-start.tmp"
            mv "$RUN/transfer-start.tmp" "$RUN/transfer-start"
            printf '%s\n' "$ACTIVE_DEADLINE" > "$RUN/active-deadline.value"
            echo MOBILE_ACTIVE_TRANSFER=true
            ui_line MOBILE_ACTIVE_TRANSFER=true
        fi
        [ "$TIMEOUT_EXPIRED" = 0 ] || break
        # UI-away is a request to close unused discovery only. Once this job
        # ever observed a file START, it cannot stop an upload or its callback.
        if [ "$ACTIVE_DEADLINE" = 0 ] && idle_cancel_requested; then
            require IDLE_CANCEL_RECEIPT_PRESENT test ! -e "$RUN/idle-cancel-consumed"
            require IDLE_CANCEL_RECEIPT_SYMLINK test ! -L "$RUN/idle-cancel-consumed"
            mv "$RUN/idle-cancel" "$RUN/idle-cancel-consumed"
            SESSION_OUTCOME=cancelled
            SESSION_RC=130
            echo MOBILE_CANCELLED=true
            ui_line MOBILE_CANCELLED=true
            ui_line MOBILE_IDLE_CANCELLED=true
            break
        fi
        if [ "$SETTLE_RESTARTED" = 1 ]; then
            phase QUICKSHARE_SETTLING
            printf 'generation=%s due_centiseconds=%s\n' "$generation" "$SETTLE_DUE" >> "$RUN/settling.txt"
        fi
        if [ "$SETTLE_READY" = 1 ]; then
                TRANSFER_COMPLETED=true
                TRANSFER_DIRECTION=${outcome#completed=}
                SESSION_OUTCOME=completed
                SESSION_RC=0
                echo MOBILE_TRANSFER_COMPLETED=true
                ui_line MOBILE_TRANSFER_COMPLETED=true
                echo QUICKSHARE_TRANSFER_COMPLETE=1
                ui_line QUICKSHARE_TRANSFER_COMPLETE=1
                ui_line "MOBILE_TRANSFER_DIRECTION=$TRANSFER_DIRECTION"
                break
        fi
        # Service destruction is distinct from PID lifetime (cached/frozen
        # processes survive onDestroy). Completed files retain their 8s grace.
        case "$outcome" in
            completed=send|completed=receive) :;;
            *)
                if provider_stop_evidence "$RUN/transfer-evidence.log"; then
                    SESSION_OUTCOME=provider_stopped
                    SESSION_RC=1
                    echo MOBILE_PROVIDER_STOPPED=true
                    ui_line MOBILE_PROVIDER_STOPPED=true
                    break
                fi
                if [ "$(pidof com.google.android.mosey 2>/dev/null)" != "$GOOGLE_PID" ]; then
                    SESSION_OUTCOME=log_unavailable
                    SESSION_RC=1
                    break
                fi;;
        esac
        if [ "$now" -ge "$PROVIDER_RENEW_AT" ]; then
            if provider_lease_renew "$now"; then :
            else
                case "$outcome" in
                    completed=send|completed=receive)
                        # Already delivered: preserve the finite callback grace.
                        PROVIDER_RENEW_AT=$((now+45));;
                    *) SESSION_OUTCOME=provider_refresh_failed; SESSION_RC=1; break;;
                esac
            fi
        fi
    else
        SESSION_OUTCOME=log_unavailable
        SESSION_RC=1
        break
    fi
    # After protocol completion, DRAINED cannot skip the Google callback grace.
    # A new START must finish and settle again; cancellation/deadline still win.
    if [ "$COMPLETION_SEEN" = 0 ] && pt_is_drained "$RUN/pt-current.txt"; then
        SESSION_OUTCOME=native_stopped
        SESSION_RC=0
        break
    fi
    sleep 2
    observation=$((observation+1))
done
printf '%s\n' "$TIMEOUT_REASON" > "$RUN/session-timeout.value"
if [ "$TIMEOUT_REASON" != none ]; then ui_line "MOBILE_SESSION_TIMEOUT=$TIMEOUT_REASON"; fi
save_outcome
RESULT_PHASE=QUICKSHARE_SESSION_FINISHED
RECOVERY_VERIFIED=false
RECOVERY_METHOD=pending
save_result
phase QUICKSHARE_SESSION_FINISHED
ui_line "MOBILE_SESSION_OUTCOME=$SESSION_OUTCOME"
else
    START_SEEN=0
    SESSION_OUTCOME=provider_refresh_failed
    SESSION_RC=1
    TRANSFER_COMPLETED=false
    TRANSFER_DIRECTION=unknown
    TIMEOUT_REASON=none
    save_outcome
    RESULT_PHASE=QUICKSHARE_SESSION_FINISHED
    RECOVERY_VERIFIED=false
    RECOVERY_METHOD=pending
    save_result
    phase QUICKSHARE_SESSION_FINISHED
    ui_line MOBILE_SESSION_OUTCOME=provider_refresh_failed
fi
binder_cmd "$BB" timeout -s TERM -k 1 8 dumpsys activity services com.google.android.mosey > "$RUN/provider-services.txt"
binder_cmd "$BB" timeout -s TERM -k 1 8 dumpsys connectivity > "$RUN/connectivity-active.txt"
phase GOOGLE_PROVIDER_STOPPING
require GOOGLE_PROVIDER_NOT_QUIET provider_quiet
# Lifetime Binder death can finish STOP; observe before sending a redundant RPC.
sleep 3
pt_snapshot "$RUN/stop-state.txt"
if pt_is_idle "$RUN/stop-state.txt"; then
    require PT_IDLE_AFTER_START_UNEXPECTED test "$START_SEEN" = 0
    require IDLE_MOSEY_INTERFACE_PRESENT test ! -e /sys/class/net/mosey0
    RADIO_EXIT=IDLE
elif pt_is_drained "$RUN/stop-state.txt"; then
    RADIO_EXIT=DRAINED
else
    require PT_NOT_SAFE_FOR_STOP pt_is_active "$RUN/stop-state.txt"
    stop_rc=0
    CLASSPATH="$RAM/qs-classes.dex" "$BB" timeout -s TERM -k 2 12 app_process /system/bin QuickShareStopProbe > "$RUN/stop-probe.txt" 2>&1 || stop_rc=$?
    printf '%s\n' "$stop_rc" > "$RUN/stop-probe-exit.value"
    for i in 1 2 3 4 5 6 7 8 9 10; do
        pt_snapshot "$RUN/stop-state.txt"
        if pt_is_drained "$RUN/stop-state.txt"; then break; fi
        sleep 1
    done
    # A concurrent lifetime-triggered STOP may beat the explicit RPC. The final
    # driver DRAINED/error=0 state is required even when the RPC returns success.
    require PT_STOP_NOT_CONFIRMED pt_is_drained "$RUN/stop-state.txt"
    RADIO_EXIT=DRAINED
fi
printf '%s\n' "$RADIO_EXIT" > "$RUN/radio-exit-branch.value"
binder_cmd "$BB" timeout -s TERM -k 1 8 dumpsys connectivity > "$RUN/connectivity-after-google-stop.txt"

phase NATIVE_THREADS_STOPPING
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
    case "$cmdtext" in *"$RAM/mosey_server"*|*' QuickShareStopProbe '*|*' BleWakeProbe '*) die RADIO_PROCESS_STILL_RUNNING;; esac
done
require MOSEY_INTERFACE_REMAINS test ! -e /sys/class/net/mosey0
require GOOGLE_PROCESS_REMAINS test -z "$(pidof com.google.android.mosey 2>/dev/null)"
require GLOBAL_MOSEY_LIBRARY_PRESENT test ! -e /system_ext/lib64/libmosey_daemon_ffi.so

framework_wifi_stop

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
pt_snapshot "$RUN/released-pt.txt"
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
dmesg | kernel_delta "$cutoff" > "$RUN/kernel-before-restore.txt"
reject PRE_RESTORE_KERNEL_FATAL grep -Eq "$FATAL" "$RUN/kernel-before-restore.txt"

phase CANDIDATE_UNLOADING
"$BB" timeout -s TERM -k 2 30 rmmod qca_cld3_kiwi_v2
require QCA_MODULE_REMAINS test ! -e /sys/module/qca_cld3_kiwi_v2
require WLAN_DEVICE_REMAINS test ! -e /dev/wlan
cat /sys/kernel/debug/cnss/stats > "$RUN/cnss-after-qca-unload.txt"
bits=$(sed -n 's/.*State: 0x\([0-9a-fA-F]*\).*/0x\1/p' "$RUN/cnss-after-qca-unload.txt")
printf '%s\n' "$bits" | grep -Eq '^0x[0-9a-fA-F]+$' || die CNSS_AFTER_UNLOAD_STATE_INVALID
cnss_gate after "$bits"
dmesg | kernel_delta "$cutoff" > "$RUN/kernel-after-qca-unload.txt"
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
echo houji_quickshare_mobile > /sys/power/wake_unlock
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
reject TEMPORARY_WAKELOCK_REMAINS grep -qw houji_quickshare_mobile /sys/power/wake_lock
dmesg > "$RUN/kernel-after.txt"
dmesg | kernel_delta "$cutoff" > "$RUN/kernel-restore-delta.txt"
reject RESTORE_KERNEL_FATAL grep -Eq "$FATAL" "$RUN/kernel-restore-delta.txt"
cmd wifi status > "$RUN/wifi-restored.txt"
RESTORED_DRIVERS=1
require RAM_CLEANUP_FAILED cleanup_unmodified_ram
require RAM_REMAINS test ! -e "$RAM"
# Commit verified drivers before cancelling the independent guard, but keep
# the shared lock until the whole guard has exited, so a new job cannot race it.
printf '%s\n' "$boot" > "$RUN/recovered-same-boot.value.tmp"
chown "$APPUID:$APPUID" "$RUN/recovered-same-boot.value.tmp"
mv "$RUN/recovered-same-boot.value.tmp" "$RUN/recovered-same-boot.value"
sync
RESTORED=1
kill -TERM "$WATCHDOG_PID" 2>/dev/null || :
for i in 1 2 3 4 5; do
    if watchdog_group_idle; then break; fi
    sleep 1
done
require WATCHDOG_CANCEL_NOT_CONFIRMED watchdog_group_idle
wait "$WATCHDOG_PID" 2>/dev/null || :
require WATCHDOG_PID_REMAINS test ! -e "/proc/$WATCHDOG_PID"
require LOCK_CLEANUP_FAILED release_session_lock
require LOCK_REMAINS test ! -e "$LOCK"
RESULT_PHASE=RECOVERED_C2_SAME_BOOT
RECOVERY_VERIFIED=true
RECOVERY_METHOD=same_boot
save_result
cp "$RUN/RUN-RESULT.json" "$RUN/status.json"
owned_logs
sync
echo MOBILE_PHASE=RECOVERED_C2_SAME_BOOT
ui_line MOBILE_PHASE=RECOVERED_C2_SAME_BOOT
exit "$SESSION_RC"
