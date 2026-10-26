#!/system/bin/sh
# Root-only management; stopping this observer never force-stops the runner.
set -eu
MOD=/data/adb/modules/houji_qs_system79
META=/metadata/houji-qs-system79
AUTO=$MOD/auto
RAM=/dev/houji-qs-auto
PATH=/system/bin:/system/xbin
export PATH
umask 077
[ "$(id -u)" = 0 ] || { echo AUTO_ERROR=ROOT_REQUIRED;exit 1; }
boot=$(cat /proc/sys/kernel/random/boot_id)
valid_observer() {
    [ -d "$RAM" ] && [ ! -L "$RAM" ] || return 1
    [ -f "$RAM/pid" ] && [ -f "$RAM/boot" ] && [ -f "$RAM/starttime" ] || return 1
    [ ! -L "$RAM/pid" ] && [ ! -L "$RAM/boot" ] && [ ! -L "$RAM/starttime" ] || return 1
    [ "$(cat "$RAM/boot")" = "$boot" ] || return 1
    pid=$(cat "$RAM/pid")
    case "$pid" in ''|*[!0-9]*) return 1;; esac
    [ "$pid" -gt 1 ] && [ -f "/proc/$pid/stat" ] || return 1
    [ "$(stat -c %u "$RAM")" = 0 ] && [ "$(stat -c %u "/proc/$pid")" = 0 ] || return 1
    value=$(cat "/proc/$pid/stat") || return 1
    rest=${value##*) }
    set -- $rest
    [ "$#" -ge 20 ] || return 1
    [ "$1" != Z ] || return 1
    shift 19
    [ "$1" = "$(cat "$RAM/starttime")" ] || return 1
    return 0
}
status() {
    if [ -f "$AUTO/enabled" ]; then echo AUTO_ENABLED=1;else echo AUTO_ENABLED=0;fi
    if valid_observer; then echo OBSERVER_RUNNING=1;else echo OBSERVER_RUNNING=0;fi
}
ready() {
    [ -f "$META/ENABLED" ] && [ ! -e "$META/DISABLED" ] && [ ! -e "$MOD/disable" ] && [ ! -e "$MOD/remove" ] || return 1
    [ -f "$META/boot-ready.boot_id" ] && [ "$(cat "$META/boot-ready.boot_id")" = "$boot" ] || return 1
    [ ! -e /dev/houji-qs-system79-management-lock ] || return 1
    return 0
}
start_observer() {
    ready || { echo AUTO_ERROR=INTEGRATION_NOT_READY;return 1; }
    if valid_observer;then status;return 0;fi
    # A single start/control lock makes concurrent boot/UI starts idempotent.
    mkdir "$RAM-control-lock" 2>/dev/null || { echo AUTO_ERROR=CONTROL_BUSY;return 1; }
    trap 'rmdir "$RAM-control-lock" 2>/dev/null || true' EXIT
    if valid_observer;then status;return 0;fi
    if [ -e "$RAM" ]; then
        [ -d "$RAM" ] && [ ! -L "$RAM" ] && [ "$(stat -c %u "$RAM")" = 0 ] || return 1
        # Delete only prior observer receipts, never jobs or shared radio locks.
        rm -f "$RAM/pid" "$RAM/boot" "$RAM/starttime" "$RAM/ready" "$RAM/stop"
    else mkdir -m 0700 "$RAM";fi
    (cd "$AUTO" && sha256sum -c auto-assets.sha256) >/dev/null || { echo AUTO_ERROR=ASSET_HASH;return 1; }
    # KSU busybox setsid detaches the idle observer; app sessions have their own watchdog.
    /data/adb/ksu/bin/busybox setsid env CLASSPATH="$AUTO/observer.dex" /system/bin/app_process /system/bin RootQuickShareObserver </dev/null >>"$RAM/observer.log" 2>&1 &
    n=0
    while ! valid_observer; do n=$((n+1));[ "$n" -le 10 ] || { echo AUTO_ERROR=OBSERVER_START_FAILED;return 1; };sleep 1;done
    status
}
case "${1:-status}" in
status) status ;;
enable)
    ready || { echo AUTO_ERROR=INTEGRATION_NOT_READY;exit 1; }
    : >"$AUTO/enabled"
    if ! start_observer;then rm -f "$AUTO/enabled";exit 1;fi
    ;;
start)
    [ -f "$AUTO/enabled" ] || { status;exit 0; }
    start_observer
    ;;
disable)
    rm -f "$AUTO/enabled"
    if valid_observer;then
        : >"$RAM/stop"
        n=0
        while valid_observer;do n=$((n+1));[ "$n" -le 20 ] || break;sleep 1;done
        if valid_observer;then
            # Only this exact PID/starttime/boot, never a process group or runner.
            kill -TERM "$pid"
        fi
    fi
    status
    ;;
*) echo 'Usage: control.sh enable|disable|status|start';exit 2 ;;
esac
