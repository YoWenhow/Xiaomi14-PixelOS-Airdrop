#!/system/bin/sh
# Exactly one bounded attempt. Remove the executable boot hook BEFORE waiting.
set -eu
META=/metadata/houji-qs-system79
SELF=/data/adb/service.d/houji-qs-system79-finalize.sh
[ "$0" = "$SELF" ] && [ ! -L "$SELF" ] && [ "$(stat -c %u "$SELF")" = 0 ] || exit 1
[ ! -L "$META" ] && [ "$(cat "$META/owner.txt")" = houji_qs_system79 ] || exit 1
cmp -s "$SELF" "$META/rollback/finalize-launcher.sh" || exit 1
if [ -e "$META/rollback-attempted" ]; then rm "$SELF";exit 0;fi
: >"$META/rollback-attempted"
sync
rm "$SELF"
exec /system/bin/timeout -s TERM -k 2 240 /system/bin/sh "$META/rollback/finalize.sh"
