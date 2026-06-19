#!/system/bin/sh

TARGET_SDK=31
TARGET_FP='Infinix/X6815B-OP/Infinix-X6815B:12/SP1A.210812.016/231020V486:user/release-keys'
TARGET_SYSTEMUI_VERSION_CODE=202309220
TARGET_SYSTEMUI_VERSION_NAME='12.0.2.187'

CURRENT_SDK="$(getprop ro.build.version.sdk)"
CURRENT_FP="$(getprop ro.build.fingerprint)"
CURRENT_ABIS="$(getprop ro.product.cpu.abilist)"
PACKAGE_INFO="$(dumpsys package com.android.systemui 2>/dev/null)"
CURRENT_SYSTEMUI_VERSION_CODE="$(printf '%s\n' "$PACKAGE_INFO" | sed -n 's/.*versionCode=\([0-9][0-9]*\).*/\1/p' | head -n 1)"
CURRENT_SYSTEMUI_VERSION_NAME="$(printf '%s\n' "$PACKAGE_INFO" | sed -n 's/.*versionName=\([^[:space:]]*\).*/\1/p' | head -n 1)"

ui_print "********************************"
ui_print " SystemUI MediaMetadata NPE Fix "
ui_print "********************************"
ui_print "* SDK: $CURRENT_SDK"
ui_print "* ABI list: $CURRENT_ABIS"
ui_print "* SystemUI: $CURRENT_SYSTEMUI_VERSION_NAME ($CURRENT_SYSTEMUI_VERSION_CODE)"
ui_print "* Fingerprint: $CURRENT_FP"

[ "$CURRENT_SDK" = "$TARGET_SDK" ] || abort "! Unsupported Android SDK: $CURRENT_SDK"
[ "$CURRENT_FP" = "$TARGET_FP" ] || abort "! Unsupported firmware fingerprint"
printf '%s' "$CURRENT_ABIS" | grep -q 'arm64-v8a' || abort "! arm64-v8a is required"
[ "$CURRENT_SYSTEMUI_VERSION_CODE" = "$TARGET_SYSTEMUI_VERSION_CODE" ] || \
  abort "! Unsupported SystemUI versionCode: $CURRENT_SYSTEMUI_VERSION_CODE"
[ "$CURRENT_SYSTEMUI_VERSION_NAME" = "$TARGET_SYSTEMUI_VERSION_NAME" ] || \
  abort "! Unsupported SystemUI versionName: $CURRENT_SYSTEMUI_VERSION_NAME"

[ -f "$MODPATH/zygisk/arm64-v8a.so" ] || abort "! Missing arm64 Zygisk library"

if [ "$APATCH" = "true" ] || [ "$KERNELPATCH" = "true" ]; then
  ui_print "* APatch/FolkPatch-compatible manager detected"
  ui_print "* A standalone Zygisk implementation is required"
elif [ -n "$KSU" ]; then
  ui_print "* KernelSU-compatible manager detected"
  ui_print "* A standalone Zygisk implementation is required"
else
  ui_print "* Magisk-compatible manager detected"
  ui_print "* Enable built-in Zygisk or use one standalone implementation"
fi

ui_print "* Keep com.android.systemui inside the Zygisk injection scope"
ui_print "* Reboot is required after installation"

set_perm_recursive "$MODPATH" 0 0 0755 0644
