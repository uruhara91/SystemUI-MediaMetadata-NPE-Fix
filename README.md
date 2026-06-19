# SystemUI MediaMetadata NPE Fix

Targeted arm64-only Zygisk workaround for this TranSystemUI crash:

```text
FATAL EXCEPTION: SysUiBg
Process: com.android.systemui
java.lang.NullPointerException
  at android.media.MediaMetadata.getString(...)
  at com.android.systemui.media.MediaDataManager.loadMediaDataInBg(...)
```

## Supported target

- Device: Infinix X6815B
- Android: 12 / API 31
- SystemUI: 12.0.2.187 (`versionCode=202309220`)
- Firmware fingerprint: `Infinix/X6815B-OP/Infinix-X6815B:12/SP1A.210812.016/231020V486:user/release-keys`
- SystemUI APK: `/system_ext/priv-app/TranSystemUI/TranSystemUI.apk`
- Target process: `com.android.systemui` via `/system/bin/app_process64`
- Packaged ABI: arm64-v8a only

The installer and native library refuse to hook a different SDK or firmware fingerprint. The installer additionally validates the exact SystemUI version. The module is intentionally specialized for this final firmware and is not a generic Android 12 fix.

## Verified runtime

The fix has been verified on the target device with FolkPatch and Zygisk Next. The native module uses only the public Zygisk API v4 hook interface and does not depend on Magisk-, Zygisk Next-, or ReZygisk-specific loader internals.

Musicolet, MiXplorer media playback, YouTube media transitions, repeated SystemUI kills, and repeated reboots no longer reproduce the null-MediaMetadata restart loop.

## How it works

The module remains mapped only in the exact `com.android.systemui` process. Other app processes and `system_server` request `DLCLOSE_MODULE_LIBRARY` immediately. The target process is never unloaded after initialization starts, preventing a dangling callback if a Zygisk implementation reports a partial PLT-hook failure.

It hooks the native `IPCThreadState::transact` Binder path and watches Android 12 transaction code 32. The common path performs one compile-time transaction-code comparison and immediately calls the original function. Descriptor parsing is delayed until a successful reply has the exact eight-byte nullable-object-null representation.

Only a validated `android.media.session.ISessionController.getMetadata` null reply is replaced with a platform-generated empty `MediaMetadata` Parcel before TranSystemUI reads it.

There is no LSPosed, LSPlant, Java method hook, modified SystemUI APK, companion daemon, or system overlay.

## v6.9 optimization and hardening

- arm64-v8a only; the target SystemUI runs through `app_process64`.
- No NDK STL runtime (`APP_STL := none`).
- Compile-time Android 12 transaction code, Binder offsets, and descriptor.
- Direct fast return for all non-matching Binder transaction codes.
- Null-reply validation before the more expensive descriptor comparison.
- One 64-bit comparison for the null reply header.
- One-time framework-generated replacement Parcel; no JNI or allocation in the Binder hook.
- Allocation-free UTF-16 process-name matching during specialization.
- Per-call JNI exception checking and clearing during initialization.
- Handle-first `libbinder.so` symbol resolution with `RTLD_DEFAULT` fallback.
- Pre-resolved original transact fallback before PLT-hook commit.
- Target library remains mapped on every target-process failure path.
- Release builds compile out informational logs; error logs remain rate-limited where needed.
- Thin LTO, section garbage collection, identical-code folding, hidden visibility, RELRO, immediate binding, non-executable stack, stack protection, and fortified libc calls.
- CI validates an arm64 ELF, dynamic Zygisk entry export, hardening, dependencies, and ZIP contents.

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
