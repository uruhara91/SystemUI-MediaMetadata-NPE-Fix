# SystemUI MediaMetadata NPE Fix

Zygisk workaround for this TranSystemUI crash:

```text
FATAL EXCEPTION: SysUiBg
Process: com.android.systemui
java.lang.NullPointerException
  at android.media.MediaMetadata.getString(...)
  at com.android.systemui.media.MediaDataManager.loadMediaDataInBg(...)
```

## How it works

When SystemUI requests MediaMetadata through
android.media.session.ISessionController.getMetadata(),
some media sessions may return a null MediaMetadata object.

TranSystemUI does not properly handle this condition and may
throw a NullPointerException while reading metadata fields.

This module intercepts only the affected Binder transaction and
replaces a verified null MediaMetadata reply with an empty
framework-generated MediaMetadata object.

The module does not modify:

- Other Binder transaction codes
- Other Binder interfaces
- Non-null MediaMetadata replies
- System_server behavior
- Application processes outside SystemUI

## Compatibility

This module is intentionally hardcoded for:

- Device: Infinix X6815B
- Android: 12 / API 31
- Firmware build: `231020V486`
- Packaged ABI: arm64-v8a only

Other firmware versions are intentionally rejected.

## Verified runtime

The fix has been verified on the target device with FolkPatch and Zygisk Next. The native module uses only the public Zygisk API v4 hook interface and does not depend on Magisk-, Zygisk Next-, or ReZygisk-specific loader internals.

Musicolet, MiXplorer media playback, YouTube media transitions, repeated SystemUI kills, and repeated reboots no longer reproduce the null-MediaMetadata restart loop.

## Design and hardening

- Low-overhead Binder transaction filtering
- Pre-generated replacement MediaMetadata parcel
- No JNI calls in the hot Binder path
- Hardened native build configuration

## Build on Windows PowerShell

Install Android NDK and set its path:

```powershell
$env:ANDROID_NDK_HOME = 'C:\Users\YOU\AppData\Local\Android\Sdk\ndk\27.2.12479018'
.\build.ps1
```

Or pass the path directly:

```powershell
.\builps1 -NdkPath 'D:\Android\ndk\27.2.12479018'
```

The flashable ZIP is written to:

```text
dist/SystemUI-Media-Fix-v6.9.zip
```

Linux builds use:

```bash
ANDROID_NDK_HOME=/path/to/ndk bash build.sh
```

## Logging

Release builds emit only errors. To enable startup and first-repair information while debugging, add this define to `LOCAL_CPPFLAGS` and rebuild:

```text
-DSYSTEMUI_MEDIA_FIX_INFO_LOGS=1
```

Then inspect:

```powershell
.\adb logcat -s SystemUIMediaFix:D AndroidRuntime:E
```

## Recovery

If SystemUI becomes unstable, disable the module and reboot:

```powershell
.\adb shell su -c 'touch /data/adb/modules/systemui_media_fix/disable'
.\adb reboot
```

It can then be removed normally from the root manager.
