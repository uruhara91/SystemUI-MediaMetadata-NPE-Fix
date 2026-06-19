#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NDK_ROOT="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
MODULE_PROP="$ROOT/module/module.prop"
VERSION="$(sed -n 's/^version=//p' "$MODULE_PROP" | head -n 1)"

if [[ -z "$NDK_ROOT" ]]; then
  echo "Set ANDROID_NDK_HOME or ANDROID_NDK_ROOT." >&2
  exit 1
fi
if [[ -z "$VERSION" ]]; then
  echo "Could not read version from $MODULE_PROP" >&2
  exit 1
fi

NDK_BUILD="$NDK_ROOT/ndk-build"
if [[ ! -x "$NDK_BUILD" ]]; then
  echo "ndk-build was not found under $NDK_ROOT" >&2
  exit 1
fi

"$NDK_BUILD" -C "$ROOT/zygisk" clean
"$NDK_BUILD" -C "$ROOT/zygisk" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"

rm -rf "$ROOT/module/zygisk"
mkdir -p "$ROOT/module/zygisk" "$ROOT/dist"
cp "$ROOT/zygisk/libs/arm64-v8a/libsystemui_media_fix.so" \
   "$ROOT/module/zygisk/arm64-v8a.so"
cp "$ROOT/zygisk/libs/armeabi-v7a/libsystemui_media_fix.so" \
   "$ROOT/module/zygisk/armeabi-v7a.so"

OUTPUT="$ROOT/dist/SystemUI-Media-Fix-${VERSION}.zip"
rm -f "$OUTPUT"
(
  cd "$ROOT/module"
  zip -r9 "$OUTPUT" .
)

echo "Built module: $OUTPUT"
