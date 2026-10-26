#!/bin/bash
# No device access; requires Clang r487747c and user-supplied Android libraries.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
clang="${CLANG:?Set CLANG to Android clang-r487747c/bin/clang}"
libs="${ANDROID_LIBS:?Directory containing libbinder_ndk.so, libc.so, libdl.so}"
libs="$(cd "$libs" && pwd)"
out="${1:?Use a fresh output directory}"
mkdir "$out"
out="$(cd "$out" && pwd)"
flags=(--target=aarch64-linux-gnu -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pie -nostdlib -static -fuse-ld=lld -Wl,-e,_start -Wall -Wextra -Werror -Wno-unused-function)
"$clang" "${flags[@]}" "$root/native/wonder-probe/probe.c" -o "$out/wonder-probe"
"$clang" "${flags[@]}" "$root/native/service/service-probe.c" -o "$out/service-probe"
"$clang" "${flags[@]}" "$root/native/scope/scope-probe.c" -o "$out/scope-probe"
"$clang" --target=aarch64-linux-android36 -O2 -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static -fuse-ld=lld -Wall -Wextra -Werror -Wl,-z,noexecstack "$root/native/bind-ro/bind-ro.c" -o "$out/bind-ro"
for variant in shim legacy-shim; do
  (
    cd "$root/native/$variant"
    "$clang" --target=aarch64-linux-android36 -O2 -mno-outline-atomics -ffreestanding -fno-builtin -fPIC -fno-stack-protector -nostdlib -shared -fuse-ld=lld -Wall -Wextra -Werror -Wl,-z,defs -Wl,--version-script=local-stability.map -Wl,-soname,local-stability.so local-stability.c rate-compat.c station-trace.c "$libs/libbinder_ndk.so" "$libs/libdl.so" "$libs/libc.so" -o "$out/$variant.so"
  )
done
sha256sum "$out/"*
