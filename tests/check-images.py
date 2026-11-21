#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check padding and architectural boot vectors for every built variant."""
import pathlib
import struct
import subprocess
import sys

for arg in sys.argv[1:]:
    image = pathlib.Path(arg)
    version, tier = map(int, image.stem.removeprefix("avd-fw-v").split("-t"))
    pad = 0xc000 if version == 2 else 0x10000 if version in (3, 4) or (version == 5 and tier == 0) else 0x12000
    data = image.read_bytes()
    assert len(data) == pad, (image, len(data), pad)
    sp, reset = struct.unpack_from("<II", data)
    # The stack starts at the SRAM end; v4 (T8122) has a 0x10000-byte code
    # window but 0x12000 bytes of SRAM above it.
    sram = 0x12000 if version == 4 else pad
    assert sp == 0x10000000 + sram, (image, sp)
    elf = image.with_suffix(".elf")
    if not elf.exists():
        elf = image.parent.parent / elf.name  # Meson >=1.10 apple/ subdirectory
    nm = subprocess.check_output(["llvm-nm", str(elf)], text=True)
    start = next(int(line.split()[0], 16) for line in nm.splitlines() if line.endswith(" T _start"))
    assert reset == start | 1, (image, hex(reset), hex(start))
    assert reset < pad and reset & 1
    print(f"{image.stem}: size={len(data):#x} SP={sp:#x} reset={reset:#x}")
if len(sys.argv) < 2:
    raise SystemExit("usage: check-images.py build/avd-fw-*.bin")
