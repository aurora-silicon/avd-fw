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
    pad = 0xc000 if version == 2 else 0x10000 if version == 3 or (version == 5 and tier == 0) else 0x12000
    data = image.read_bytes()
    assert len(data) == pad, (image, len(data), pad)
    codecs = b"CODECS:h265,h264,vp9" + (b",av1" if version >= 4 else b"")
    assert data[-64:] == codecs.ljust(64, b"\0"), (image, "codec manifest")
    sp, reset = struct.unpack_from("<II", data)
    assert sp == 0x10000000 + pad, (image, sp)
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
