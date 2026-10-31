#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."
for variant in 2:0 3:0 3:1 3:2 4:0 5:0 5:1 5:2; do
    version=${variant%:*}
    tier=${variant#*:}
    make -B AVD_VER="$version" AVD_TIER="$tier"
done
