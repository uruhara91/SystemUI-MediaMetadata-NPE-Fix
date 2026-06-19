#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLCHAIN="${ANDROID_NDK_HOME:?ANDROID_NDK_HOME is required}/toolchains/llvm/prebuilt/linux-x86_64/bin"
READELF="$TOOLCHAIN/llvm-readelf"

if [[ ! -x "$READELF" ]]; then
  echo "llvm-readelf was not found: $READELF" >&2
  exit 1
fi

SO="$ROOT/zygisk/libs/arm64-v8a/libsystemui_media_fix.so"
[[ -f "$SO" ]] || { echo "Missing library: $SO" >&2; exit 1; }
[[ ! -e "$ROOT/zygisk/libs/armeabi-v7a" ]] || {
  echo "Unexpected 32-bit build output detected" >&2
  exit 1
}

HEADER="$($READELF -hW "$SO")"
PROGRAM="$($READELF -lW "$SO")"
DYNAMIC="$($READELF -dW "$SO")"
DYNSYMS="$($READELF --dyn-syms -W "$SO")"

printf '%s\n' "$HEADER" | grep -q 'Class:[[:space:]]*ELF64' || {
  echo "Expected ELF64 output" >&2
  exit 1
}
printf '%s\n' "$HEADER" | grep -q 'Machine:[[:space:]]*AArch64' || {
  echo "Expected AArch64 output" >&2
  exit 1
}
printf '%s\n' "$DYNSYMS" | grep -Eq \
  '[[:space:]]GLOBAL[[:space:]]+DEFAULT[[:space:]]+[0-9]+[[:space:]]+zygisk_module_entry$' || {
  echo "Missing exported zygisk_module_entry in .dynsym" >&2
  exit 1
}
printf '%s\n' "$PROGRAM" | grep -q 'GNU_RELRO' || {
  echo "GNU_RELRO is missing" >&2
  exit 1
}
printf '%s\n' "$DYNAMIC" | grep -Eq 'BIND_NOW|FLAGS_1.*NOW' || {
  echo "Immediate binding is missing" >&2
  exit 1
}
if printf '%s\n' "$PROGRAM" | grep -Eq 'GNU_STACK.*RWE'; then
  echo "Executable stack detected" >&2
  exit 1
fi
if printf '%s\n' "$DYNAMIC" | grep -q 'TEXTREL'; then
  echo "TEXTREL detected" >&2
  exit 1
fi
if printf '%s\n' "$DYNAMIC" | grep -Eq 'RPATH|RUNPATH'; then
  echo "Unexpected RPATH/RUNPATH detected" >&2
  exit 1
fi
if printf '%s\n' "$DYNAMIC" | grep -q 'libc++_shared.so'; then
  echo "Unexpected shared libc++ dependency" >&2
  exit 1
fi

echo "arm64-v8a: ELF verification passed"
