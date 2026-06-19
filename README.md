# SystemUI MediaMetadata NPE Fix

Targeted Zygisk-only workaround for this TranSystemUI crash:

```text
FATAL EXCEPTION: SysUiBg
Process: com.android.systemui
java.lang.NullPointerException
  at com.android.systemui.media.MediaDataManager.loadMediaDataInBg(...)
```

## Supported target

- Device: Infinix X6815B
- Android: 12 / API 31
- SystemUI: 12.0.2.187 (`versionCode=202309220`)
- Firmware fingerprint: `Infinix/X6815B-OP/Infinix-X6815B:12/SP1A.210812.016/231020V486:user/release-keys`
- SystemUI APK: `/system_ext/priv-app/TranSystemUI/TranSystemUI.apk`
- ABIs packaged: arm64-v8a and armeabi-v7a

The installer and native library refuse to hook a different SDK or firmware fingerprint. The installer additionally validates the exact SystemUI version.

## Verified runtime

The fix has been verified on the target device with:

```text
FolkPatch + ZygiskNext
```

Musicolet, MiXplorer media playback, and closing the YouTube miniplayer no longer restart SystemUI.

## How it works

The module remains loaded only in `com.android.systemui`. Other processes request `DLCLOSE_MODULE_LIBRARY` immediately.

It hooks the native `IPCThreadState::transact` Binder path and watches only validated `android.media.session.ISessionController.getMetadata` calls. When the media session returns nullable `MediaMetadata` as `null`, the module replaces that reply with a platform-generated empty `MediaMetadata` Parcel before TranSystemUI reads it.

There is no LSPosed, LSPlant, Java method hook, modified SystemUI APK, companion daemon, or system overlay.

## v0.3.0 optimization

- Compile-time Android 12 Binder offsets for the fingerprint-locked target.
- Fast transaction-code rejection before descriptor parsing.
- Bounds, descriptor, terminator, reply-header, and generated-Parcel validation.
- Process-lifetime fixed buffer instead of `std::vector`.
- Replacement logging only once per SystemUI process.
- One-time JNI work during SystemUI startup; no JNI in the Binder hot path.
- Thin LTO, section garbage collection, identical-code folding, hidden visibility, RELRO, immediate binding, stack protection, and fortified libc calls.
- Build artifact version is derived from `module/module.prop`.

## Build on Windows PowerShell

Install Android NDK and set its path:

```powershell
$env:ANDROID_NDK_HOME = 'C:\Users\YOU\AppData\Local\Android\Sdk\ndk\27.2.12479018'
.\build.ps1
```

Or pass the path directly:

```powershell
.\build.ps1 -NdkPath 'D:\Android\ndk\27.2.12479018'
```

The flashable ZIP is written to:

```text
dist/SystemUI-Media-Fix-v0.3.0.zip
```

## Test

After flashing and rebooting:

```powershell
.\adb logcat -c
.\adb logcat -s SystemUIMediaFix:D AndroidRuntime:E
```

Expected startup messages:

```text
SystemUIMediaFix: Prepared empty MediaMetadata reply
SystemUIMediaFix: Hook installed
```

The first repaired response in each SystemUI process logs:

```text
SystemUIMediaFix: Replaced null MediaMetadata Binder reply
```

## Recovery

If SystemUI becomes unstable, disable the module and reboot:

```powershell
.\adb shell su -c 'touch /data/adb/modules/systemui_media_fix/disable'
.\adb reboot
```

It can then be removed normally from the root manager.

## PowerShell note

Use PowerShell's own filtering:

```powershell
.\adb shell dumpsys package com.android.systemui |
    Select-String 'versionName|versionCode'
```

Or execute grep inside Android's shell:

```powershell
.\adb shell "dumpsys package com.android.systemui | grep -E 'versionName|versionCode'"
```
