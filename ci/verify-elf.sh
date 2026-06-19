#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
READELF="${ANDROID_NDK_HOME:?ANDROID_NDK_HOME is required}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"

if [[ ! -x "$READELF" ]]; then
  echo "llvm-readelf was not found: $READELF" >&2
  exit 1
fi

verify_one() {
  local abi="$1"
  local so="$ROOT/zygisk/libs/$abi/libsystemui_media_fix.so"

  [[ -f "$so" ]] || { echo "Missing library: $so" >&2; exit 1; }

  "$READELF" -Ws "$so" | grep -q 'zygisk_module_entry' || {
    echo "$abi: missing exported zygisk_module_entry" >&2
    exit 1
  }

  "$READELF" -l "$so" | grep -q 'GNU_RELRO' || {
    echo "$abi: GNU_RELRO is missing" >&2
    exit 1
  }

  "$READELF" -d "$so" | grep -Eq 'BIND_NOW|FLAGS_1.*NOW' || {
    echo "$abi: immediate binding is missing" >&2
    exit 1
  }

  if "$READELF" -d "$so" | grep -q 'TEXTREL'; then
    echo "$abi: TEXTREL detected" >&2
    exit 1
  fi

  if "$READELF" -d "$so" | grep -q 'libc++_shared.so'; then
    echo "$abi: unexpected shared libc++ dependency" >&2
    exit 1
  fi

  echo "$abi: ELF verification passed"
}

verify_one arm64-v8a
verify_one armeabi-v7a
