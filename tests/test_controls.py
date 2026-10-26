from pathlib import Path
import subprocess,tempfile,json,hashlib,shutil,xml.etree.ElementTree as ET
HERE=Path(__file__).resolve().parent
PROJECT=HERE.parent
APP=PROJECT/'app'
helper=(HERE/'controls-stop.inc').read_text()
checks={};scenarios={}
def check(name,value):
    assert value,name
    checks[name]=True
fixture=HERE/'fixtures/wifi-disabled.txt'
parser="sed -n -e 's/^Wifi is enabled$/enabled/p' -e 's/^Wifi is disabled$/disabled/p'"
check('portable_two_expression_parser',parser in helper and '\\|' not in helper)
fixtures=[('real_toybox_probe_disabled',fixture.read_text(),'disabled\n'),('enabled','Wifi is enabled\nWifi scanning is always available\n','enabled\n'),('unknown','Wifi is disabling\n',''),('partial','Wifi is disabled later\n',''),('prefixed',' Wifi is disabled\n',''),('contradictory','Wifi is enabled\nWifi is disabled\n','enabled\ndisabled\n')]
for name,payload,expected in fixtures:
    p=subprocess.run(['bash','-c',parser],input=payload,capture_output=True,text=True,check=True)
    check('parser_'+name,p.stdout==expected)

mock=r'''
set -eu
RUN=$1; SCENARIO=$2; BB=mock_bb
echo 0 > "$RUN/clock"
die() { echo "ERROR=$1"; exit 10; }
require() { label=$1;shift; "$@" || die "$label"; }
binder_cmd() { "$@"; }
uptime_seconds() { cat "$RUN/clock"; }
sleep() { t=$(cat "$RUN/clock"); echo $((t+1)) > "$RUN/clock"; }
mock_state() {
  SIM_FW=disabled SIM_WIFI=0 SIM_SCAN=0 SIM_CND=stopped SIM_HAL=running
  if [ "$CTRL_STAGE" = hal ]; then SIM_HAL=stopped; fi
  case "$SCENARIO" in
    delayed_close)
      if [ "$CTRL_SAMPLE" -le 3 ]; then SIM_FW=enabled;SIM_WIFI=1;SIM_SCAN=1;SIM_CND=stopping;fi
      if [ "$CTRL_STAGE" = hal ] && [ "$CTRL_SAMPLE" -eq 6 ]; then SIM_HAL=stopping;fi;;
    hal_restart)
      if [ "$CTRL_STAGE" = hal ]; then
        case "$CTRL_SAMPLE" in 3) SIM_HAL=running;;4) SIM_HAL=restarting;;esac
      fi;;
    hal_never_stops) SIM_HAL=running;;
    cnd_never_stops) SIM_CND=running;;
    framework_never_disables) SIM_FW=enabled; SIM_WIFI=1;;
    policy_never_disables) SIM_WIFI=1;;
    scan_never_disables) SIM_SCAN=1;;
    framework_reenabled)
      if [ "$CTRL_STAGE" = hal ];then SIM_FW=enabled;SIM_WIFI=1;fi;;
    hal_unknown) SIM_HAL=unknown;;
    cnd_unknown) SIM_CND=unknown;;
    policy_unknown) SIM_WIFI=null;;
    scan_unknown) SIM_SCAN=null;;
    framework_bounce)
      if [ "$CTRL_STAGE" = framework ] && [ "$CTRL_SAMPLE" -eq 2 ];then SIM_FW=enabled;SIM_WIFI=1;fi;;
    hal_bounce)
      if [ "$CTRL_STAGE" = hal ] && [ "$CTRL_SAMPLE" -eq 4 ];then SIM_HAL=running;fi;;
  esac
}
mock_bb() {
  [ "$1" = timeout ] && [ "$2" = -s ] && [ "$3" = TERM ] && [ "$4" = -k ] && [ "$5" = 1 ] || exit 99
  delay=$6;shift 6
  case "$delay" in 3|8) :;;*) exit 98;;esac
  printf 'stage=%s sample=%s command=%s\n' "$CTRL_STAGE" "$CTRL_SAMPLE" "$*" >> "$RUN/commands"
  if [ "$SCENARIO" = command_timeout ] && [ "$*" = 'svc wifi disable' ];then return 124;fi
  if [ "$SCENARIO" = command_error ] && [ "$*" = 'cmd wifi set-scan-always-available disabled' ];then echo denied >&2;return 1;fi
  if [ "$SCENARIO" = status_error ] && [ "$*" = 'cmd wifi status' ];then echo binder-failure >&2;return 1;fi
  if [ "$SCENARIO" = status_timeout ] && [ "$*" = 'cmd wifi status' ];then return 124;fi
  if [ "$SCENARIO" = settings_error ] && [ "$*" = 'settings get global wifi_on' ];then echo settings-failure >&2;return 1;fi
  if [ "$delay" = 3 ] && [ "$SCENARIO" = slow_reads ];then t=$(cat "$RUN/clock");echo $((t+2)) > "$RUN/clock";fi
  mock_state
  case "$*" in
    'stop vendor.cnd'|'stop vendor.wifi_hal_legacy'|'cmd wifi set-scan-always-available disabled'|'svc wifi disable') return 0;;
    'cmd wifi status') if [ "$SCENARIO" = framework_unknown ];then echo 'No framework';else echo "Wifi is $SIM_FW";fi;;
    'settings get global wifi_on') echo "$SIM_WIFI";;
    'settings get global wifi_scan_always_enabled') echo "$SIM_SCAN";;
    'getprop init.svc.vendor.cnd') echo "$SIM_CND";;
    'getprop init.svc.vendor.wifi_hal_legacy') echo "$SIM_HAL";;
    *) echo "UNEXPECTED=$*" >&2; return 97;;
  esac
}
'''
cases={
 'immediate':None,'delayed_close':None,'hal_restart':None,'framework_bounce':None,'hal_bounce':None,
 'hal_never_stops':'CONTROLS_HAL_STOP_TIMEOUT','cnd_never_stops':'CONTROLS_HAL_STOP_TIMEOUT',
 'framework_never_disables':'CONTROLS_FRAMEWORK_DISABLE_TIMEOUT','policy_never_disables':'CONTROLS_FRAMEWORK_DISABLE_TIMEOUT','scan_never_disables':'CONTROLS_FRAMEWORK_DISABLE_TIMEOUT',
 'framework_reenabled':'CONTROLS_FRAMEWORK_REENABLED','hal_unknown':'CONTROLS_HAL_STATE_UNKNOWN','cnd_unknown':'CONTROLS_CND_STATE_UNKNOWN','policy_unknown':'CONTROLS_WIFI_POLICY_UNKNOWN','scan_unknown':'CONTROLS_SCAN_POLICY_UNKNOWN','framework_unknown':'CONTROLS_FRAMEWORK_STATE_UNKNOWN',
 'command_timeout':'CONTROLS_STOP_COMMAND_FAILED','command_error':'CONTROLS_STOP_COMMAND_FAILED','status_error':'CONTROLS_STOP_STATE_READ_FAILED','status_timeout':'CONTROLS_STOP_STATE_READ_FAILED','settings_error':'CONTROLS_STOP_STATE_READ_FAILED','slow_reads':'CONTROLS_HAL_STOP_TIMEOUT',
}
for scenario,error in cases.items():
    with tempfile.TemporaryDirectory() as tmp:
        p=subprocess.run(['bash','-c',mock+helper+'\ncontrols_stop_synchronized\necho UNLOAD_ADMITTED\n','test',tmp,scenario],capture_output=True,text=True,timeout=10)
        d=Path(tmp);history=(d/'controls-stop-history.txt').read_text();commands=(d/'commands').read_text()
        if error is None:
            check(scenario,p.returncode==0 and 'UNLOAD_ADMITTED' in p.stdout)
            final=[line for line in history.splitlines() if 'phase=hal' in line][-2:]
            check(scenario+'_two_fully_down_samples',len(final)==2 and all('framework=disabled wifi_on=0 scan=0 cnd=stopped hal=stopped' in line for line in final))
        else:
            check(scenario,p.returncode==10 and 'ERROR='+error in p.stdout and 'UNLOAD_ADMITTED' not in p.stdout)
        stops=commands.count('command=stop vendor.wifi_hal_legacy\n')
        check(scenario+'_bounded_hal_stop',stops<=3)
        check(scenario+'_preserves_evidence',bool(history) and (bool(list(d.glob('controls-*-*.txt')))))
        if scenario in ('framework_never_disables','policy_never_disables','scan_never_disables','framework_unknown'):
            check(scenario+'_never_stops_hal_before_framework',stops==0)
        scenarios[scenario]={'exit':p.returncode,'stdout':p.stdout,'stderr':p.stderr,'history':history,'commands':commands,'hal_stop_count':stops,'mock_elapsed_seconds':int((d/'clock').read_text())}
        if scenario=='hal_restart':check('restart_resends_stop_at_most_three_times',stops==3)

(HERE/'CONTROLS-TESTS.json').write_text(json.dumps(dict(passed=True,scenarios=len(cases),checks=len(checks)),indent=2))
print('CONTROLS_SCENARIOS_PASSED='+str(len(cases)))
