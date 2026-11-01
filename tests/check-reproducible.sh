#!/bin/sh
# SPDX-License-Identifier: MIT
# Build every image with the Makefile and with Meson (llvm.ini, default options)
# and require byte-identical results, so there is one reproducible firmware.
set -eu
cd "$(dirname "$0")/.."
out=build/repro
rm -rf "$out"
mkdir -p "$out/make"
for variant in 2:0 3:0 3:1 4:0 5:0 5:1 5:2; do
    version=${variant%:*}
    tier=${variant#*:}
    make -s AVD_VER="$version" AVD_TIER="$tier"
    cp "build/avd-fw-v$version-t$tier.bin" "$out/make/"
done
env -u CFLAGS -u LDFLAGS meson setup "$out/meson" --cross-file llvm.ini >/dev/null
meson compile -C "$out/meson" >/dev/null
fail=0
for image in "$out"/make/avd-fw-*.bin; do
    name=$(basename "$image")
    other=$(find "$out/meson" -name "$name" -type f | head -n 1)
    if [ -z "$other" ]; then
        echo "$name: missing from the Meson build"; fail=1
    elif cmp -s "$image" "$other"; then
        echo "$name: identical $(sha256sum "$image" | cut -d' ' -f1)"
    else
        echo "$name: Make and Meson images differ"; fail=1
    fi
done
exit $fail
