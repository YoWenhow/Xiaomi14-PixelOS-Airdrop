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

