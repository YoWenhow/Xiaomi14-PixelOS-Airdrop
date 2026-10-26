#!/system/bin/sh
# Sourced by KernelSU. Keep traps and shell options inside the child helper.
[ "$KSU" = true ] || abort '! KernelSU is required'
[ "$BOOTMODE" = true ] || abort '! Install from a booted Android system'
case "$MODPATH" in
  /data/adb/modules_update/houji_qs_system79) ;;
  *) abort '! Unexpected KernelSU staging directory' ;;
esac
set_perm_recursive "$MODPATH" 0 0 0755 0644
for f in "$MODPATH"/*.sh "$MODPATH"/auto/*.sh "$MODPATH"/installer/*.sh; do set_perm "$f" 0 0 0755; done
ui_print '- Checking this ROM and installing the dedicated AirDrop integration'
if /system/bin/sh "$MODPATH/installer/setup.sh" "$MODPATH"; then
  ui_print '- Installed. Reboot, then grant Root to the companion app and run its check.'
  ui_print '- Automatic Quick Share entry remains off until enabled in the companion app.'
else
  abort '! Installation failed; see /metadata/houji-qs-system79/install.log if present'
fi
