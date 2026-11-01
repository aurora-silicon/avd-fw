#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run built CM3 transport code against a synthetic MMIO/PIODMA model.

Requires the optional `unicorn` Python package and llvm-nm on PATH.
This checks software integration, not silicon register semantics.
"""
import pathlib
import struct
import subprocess
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC


class Transport:
    def __init__(self, image):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.uc.mem_map(0, 0x20000)
        self.uc.mem_map(0x10000000, 0x20000)
        self.uc.mem_map(0x40000000, 0x200000)
        self.uc.mem_map(0x41000000, 0x200000)
        self.uc.mem_map(0x50010000, 0x4000)
        self.uc.mem_map(0xe000e000, 0x2000)
        self.uc.mem_write(0, image.read_bytes())
        elf = image.with_suffix(".elf")
        if not elf.exists():
            elf = image.parent.parent / elf.name
        nm = subprocess.check_output(["llvm-nm", str(elf)], text=True)
        self.symbol = {name: int(addr, 16) for addr, kind, name in
                       (line.split() for line in nm.splitlines() if len(line.split()) == 3)}
        self.version, self.tier = map(int, image.stem.removeprefix("avd-fw-v").split("-t"))
        self.decode = 0x40100000 if self.version <= 3 else 0x41100000
        self.dma = 0x40070000 if self.version <= 3 else 0x41070000
        self.fifo = 0x4004 if self.version == 2 else 0xc
        self.pp = 0x4014 if self.version == 2 else 0x30 if self.version == 3 else 0x40
        self.maximum = 0x4018 if self.version == 2 else 0x34 if self.version == 3 else 0x44
        self.current = 0x402c if self.version == 2 else 0x5c if self.version == 3 else 0x7c
        self.vps = 4 if self.version == 2 else 9 if self.version == 3 else 12
        self.pp_idx = 4 if self.version == 2 else 9 if self.version == 3 else 13
        self.packets = {}
        self.stores, self.kicks, self.notifications, self.fetches = [], [], [], []
        self.engine_stores = []
        self.busy, self.full, self.dma_ticks, self.dma_timeout = False, False, 0, False
        self.clear = set()
        self.ring_writes = set()
        self.uc.hook_add(UC_HOOK_MEM_READ, self.read)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.write)
        self.call("avdq_fw_init")
        self.info = [self.u32(0x10007fc0 + i * 4) for i in range(16)]
        assert self.info[:4] == [0x41565131, 2, 16, 64]
        assert self.info[11:16] == [15, 0, 0x1800, 0, 0]
        expected = 0x0108c000 if self.version == 2 else 0x01090000 if self.version == 3 or (self.version == 5 and self.tier == 0) else 0x01094000 if (self.version == 5 and self.tier == 2) else 0x01092000
        assert self.info[4] == expected and self.info[8] == self.vps
        assert self.info[10] == (7 if self.version == 2 or (self.version == 5 and self.tier == 0) else 15)
        self.ring = self.info[4] + self.info[5]
        pool = [self.u32(0x10007f80 + i * 4) for i in range(8)]
        assert pool[:2] == [0x41564d32, 1] and pool[7] == 0
        self.pool = pool[2:6]
        assert pool[6] == (15 if (self.version, self.tier) == (5, 2) else 0)
        expected_pool = [3, 4, 8, 0] if self.version == 2 else [15, 240, 256, 0] if self.version == 3 else [15, 240, 256, 1536] if self.version == 4 else [3, 16, 256, 512] if self.tier == 0 else [15, 240, 256, 3584]
        assert self.pool == expected_pool

    def u32(self, addr):
        return struct.unpack("<I", self.uc.mem_read(addr, 4))[0]

    def put(self, addr, value):
        self.uc.mem_write(addr, struct.pack("<I", value))

    def call(self, name, a=0, b=0):
        self.uc.reg_write(UC_ARM_REG_SP, 0x1000b000)
        self.uc.reg_write(UC_ARM_REG_R0, a)
        self.uc.reg_write(UC_ARM_REG_R1, b)
        self.uc.reg_write(UC_ARM_REG_LR, 0x1f001)
        self.uc.emu_start(self.symbol[name] | 1, 0x1f000, count=1000000)
        assert self.uc.reg_read(UC_ARM_REG_PC) == 0x1f000, name + " did not return"
        return self.uc.reg_read(UC_ARM_REG_R0)

    def read(self, uc, access, addr, size, value, user):
        if addr in self.clear:
            self.put(addr, 0)
            self.clear.remove(addr)
        if addr == 0x5001005c:
            self.put(addr, 0x10101 if self.busy else 0x20001)
        if self.decode + self.maximum <= addr < self.decode + self.maximum + self.vps * 4:
            self.put(addr, 256)
        if self.decode + self.current <= addr < self.decode + self.current + self.vps * 4:
            self.put(addr, 256 if self.full else 0)
        if addr == self.dma + 4:
            if self.dma_timeout:
                self.put(addr, 0)
            elif self.dma_ticks:
                self.dma_ticks -= 1
                self.put(addr, int(self.dma_ticks == 0))

    def write(self, uc, access, addr, size, value, user):
        if 0x10000000 <= addr < 0x10006000:
            self.ring_writes.add(addr)  # firmware stores into the host ring
        if addr == self.dma + 0x54:
            source = self.u32(self.dma + 0x4c) | self.u32(self.dma + 0x50) << 32
            header, payload = self.packets[source]
            assert header == self.info[7]
            size_bytes = (value << 2) >> 8  # public emulator command decode
            assert size_bytes == 4 * (len(payload) + 1)
            assert value & 0xff == 0x11
            self.fetches.append(source)
            self.uc.mem_write(0x10009000, struct.pack("<" + "I" * len(payload), *payload))
            self.dma_ticks = 3
            self.put(self.dma + 4, 0)
        if self.decode + self.fifo <= addr < self.decode + self.fifo + self.vps * 4:
            self.stores.append(value)
            self.engine_stores.append(((addr - self.decode - self.fifo) // 4, value))
        if addr == self.decode + self.pp:
            self.kicks.append(value)
        if addr == 0x50010060:
            assert not self.busy, "unread notification overwritten"
            self.busy = True
            self.notifications.append(value)
        # irq_clear() writes W1C decode status then waits for the ack.
        status = self.decode + (0x4060 if self.version == 2 else 0x124 if self.version == 3 else 0x194)
        if status <= addr <= status + 13 * 4:
            self.clear.add(addr)

    def submit(self, idx, tag, words, vp=0):
        source = 0x100004000 + idx * 0x10000
        for p in range((len(words) + 255) // 256):
            self.packets[source + p * 1028] = (self.info[7], words[p * 256:(p + 1) * 256])
        desc = [0x41565131, tag, 0, vp, source & 0xffffffff, source >> 32,
                len(words), 0x2b000400 | idx << 4, 1, 0, 0, 0, 0, 0, 0, 0]
        self.uc.mem_write(0x10008000 + idx * 64, struct.pack("<16I", *desc))
        # Exercise the actual mailbox IRQ wrapper and interrupt acknowledgement.
        self.put(0x50010058, self.ring + idx * 64)
        self.call("irq1")

    def poll(self, n=1):
        for _ in range(n):
            self.call("avdq_fw_poll")

    def vp_done(self):
        self.call("irq20" if self.version == 2 else "irq80")

    def pp_done(self):
        self.call("irq40" if self.version == 2 else "irq64")

    def result(self, idx):
        return [self.u32(0x10008000 + idx * 64 + off) for off in (0x20, 0x24, 0x28)]


RING_WORDS = 0x1800


def ring_write(t, start, words):
    for i, w in enumerate(words):
        t.put(0x10000000 + 4 * ((start + i) % RING_WORDS), w)


def submit_sram(t, idx, tag, start, count, extra=0, cont=0, vp=0, codec=0, flags=1):
    desc = [0x41565131, tag, codec, vp, start & 0xffffffff, 0, count,
            0x2b000400 | idx << 4, 1, 0, 0, flags, extra, cont, 0, 0]
    t.uc.mem_write(0x10008000 + idx * 64, struct.pack("<16I", *desc))
    t.put(0x50010058, t.ring + idx * 64)
    t.call("irq1")


def check_sram(image):
    t = Transport(image)
    a = list(range(0x100, 0x100 + 100))
    b = [0xb0000000 | i for i in range(7000)]  # larger than the ring, wraps
    ring_write(t, 0, a)
    t.put(0x10007ffc, len(a))
    submit_sram(t, 0, 0x50000, 0, len(a), extra=2, cont=0x2b000010)
    submit_sram(t, 1, 0x50011, len(a), len(b))
    t.poll(4)
    assert t.stores == a and t.u32(0x10007ff8) == len(a)
    t.vp_done(); t.poll()
    assert t.kicks == [0x2b000400, 0x2b000010, 0x2b000010]
    # Stream B: the host may only run RING_WORDS ahead of the firmware.
    wp = len(a)
    while wp < len(a) + len(b):
        rp = t.u32(0x10007ff8)
        n = min(RING_WORDS - (wp - rp), len(a) + len(b) - wp, 1500)
        ring_write(t, wp, b[wp - len(a):wp - len(a) + n])
        wp += n
        t.put(0x10007ffc, wp)
        t.poll(40)
    t.full = True
    t.poll(3)
    t.full = False
    t.poll(200)
    assert t.stores == a + b, (len(t.stores), len(a) + len(b))
    assert t.u32(0x10007ff8) == len(a) + len(b)
    assert not t.fetches
    t.vp_done(); t.poll()
    t.pp_done(); t.poll()
    assert t.result(0) == [2, 0, 0x50000] and t.notifications == [0x54530000]
    t.pp_done(); t.poll()  # stray PP DONE from a multi-kick frame is ignored
    assert t.kicks[-1] == 0x2b000410
    t.pp_done(); t.busy = False; t.poll()
    assert t.result(1) == [2, 0, 0x50011]
    assert t.notifications == [0x54530000, 0x54530011]
    assert not t.ring_writes, "firmware wrote into the SRAM ring: %s" % sorted(map(hex, t.ring_writes))
    # A descriptor whose start disagrees with the ring position is fatal.
    t.busy = False
    rp = t.u32(0x10007ff8)
    ring_write(t, rp, [1, 2])
    t.put(0x10007ffc, rp + 2)
    submit_sram(t, 2, 0x60002, rp + 1, 2)
    t.poll(4); t.busy = False; t.poll(2)
    assert t.result(2) == [2, 2, 0x60002]
    print(image.stem + ": SRAM ring, wrap, streaming, multi-kick, stray PP DONE, position check passed", flush=True)


def check(image):
    t = Transport(image)
    # Legacy outputs before the first queue doorbell are unchanged.
    t.vp_done()
    assert t.notifications == [0x100]
    t.busy = False
    t.pp_done()
    assert t.notifications[-1] == 0x1000
    t.busy = False
    t.notifications.clear()
    words = list(range(513))
    t.submit(0, 0xabcd0000, words)
    t.submit(1, 0x12340001, [0xfeed])
    t.full = True
    t.poll(20)
    assert not t.stores and len(t.fetches) == 1
    t.full = False
    t.poll(550)
    assert t.stores == words
    assert len(t.fetches) == 3 and not t.kicks and not t.notifications
    t.vp_done(); t.poll()
    t.poll(8)
    assert t.stores == words + [0xfeed] and len(t.fetches) == 4
    assert t.kicks == [0x2b000400] and not t.notifications
    t.vp_done(); t.poll()
    assert len(t.kicks) == 1
    t.pp_done(); t.poll()
    assert t.result(0) == [2, 0, 0xabcd0000]
    assert t.notifications == [0x54530000]
    assert t.kicks == [0x2b000400, 0x2b000410]
    t.pp_done(); t.poll(5)
    assert t.result(1)[0] == 1 and len(t.notifications) == 1
    t.busy = False; t.poll()
    assert t.result(1) == [2, 0, 0x12340001]
    assert t.notifications == [0x54530000, 0x54530001]
    # Exercise slot reuse with a different full generation and max u32 tag.
    t.busy = False
    t.submit(15, 0xffffffff, [123])
    t.poll(8); t.vp_done(); t.poll(); t.pp_done(); t.poll()
    assert t.result(15) == [2, 0, 0xffffffff]
    assert t.notifications[-1] == 0x5453ffff
    # Decode error halts all accepted work; late events cannot kick PP.
    t = Transport(image)
    t.submit(0, 0x20000, [1]); t.submit(1, 0x20001, [2]); t.poll(20)
    t.call("irq19" if t.version == 2 else "irq79")
    t.poll(); t.busy = False; t.poll()
    assert t.result(0) == [2, 2, 0x20000] and t.result(1) == [2, 2, 0x20001]
    t.vp_done(); t.poll()
    assert not t.kicks
    # A duplicate doorbell must not replace an in-flight tag; it poisons the
    # queue and emits the separate fatal token after accepted errors drain.
    t = Transport(image)
    t.submit(0, 0x20000, [1]); t.poll(8)
    t.put(0x50010058, t.ring); t.call("irq1")
    t.poll(2)
    assert t.result(0) == [2, 2, 0x20000]
    t.busy = False; t.poll()
    assert t.notifications[-1] == 0x5452ffff
    # Explicit init is also sufficient after dirty SRAM (boot/reset path).
    t.call("avdq_fw_init")
    t.busy = False; t.notifications.clear()
    t.submit(0, 0x30000, [3]); t.poll(8)
    t.vp_done(); t.poll(); t.pp_done(); t.poll()
    assert t.result(0) == [2, 0, 0x30000]
    print(image.stem + ": legacy IRQs, 64-bit PIODMA, packets, FIFO pressure, VP->PP, outbox, tags, error/reset passed", flush=True)


def check_multi(image):
    # Every engine of every supported codec, with reverse-order VP DONE.
    for codec in range(4):
        t = Transport(image)
        engines = [vp for vp in range(t.vps) if t.pool[codec] & (1 << vp)]
        if not engines:
            continue
        for idx, vp in enumerate(engines):
            ring_write(t, idx, [0xc0000000 | idx])
            submit_sram(t, idx, 0xa0000 | idx, idx, 1, vp=vp, codec=codec, flags=3)
        t.put(0x10007ffc, len(engines))
        t.poll(20)
        assert t.engine_stores == [(vp, 0xc0000000 | idx) for idx, vp in enumerate(engines)]
        assert not t.kicks and not t.notifications
        # Hold the outbox while all engine IRQs arrive. Check both routers.
        t.busy = True
        for idx in reversed(range(len(engines))):
            vp = engines[idx]
            irq = (18 if t.version == 2 else 78) + 5 * vp + 2
            bank, bit = divmod(irq - 14, 32)
            assert t.u32(0x50010014 + bank * 4) & (1 << bit)
            t.call('irq' + str(irq))
            t.poll()
            assert t.result(idx)[0] == 1
            if idx:
                assert not t.kicks, 'PP bypassed an earlier VP'
        assert not t.notifications
        # Drain one tagged VP event per engine without releasing its descriptor.
        for _ in engines:
            t.busy = False
            t.poll()
        assert t.notifications == [0x54540000 | idx for idx in range(len(engines))]
        for idx in range(len(engines)):
            assert t.u32(0x10008038 + idx * 64) == (0xa0000 | idx)
            assert t.u32(0x1000803c + idx * 64) == 1
            assert t.result(idx)[0] == 1
        # PP and full completions stay submission ordered, even with outbox stalls.
        for idx in range(len(engines)):
            assert len(t.kicks) == idx + 1
            assert t.kicks[idx] == 0x2b000400 | idx << 4
            t.pp_done(); t.poll()
            assert t.result(idx)[0] == 1
            t.busy = False; t.poll()
            assert t.result(idx) == [2, 0, 0xa0000 | idx]
        assert t.notifications[len(engines):] == [0x54530000 | idx for idx in range(len(engines))]
    # Reject a codec/engine mismatch, including a tier-0 hole, before feeding.
    t = Transport(image)
    bad = next(vp for vp in range(t.vps) if not t.pool[1] & (1 << vp))
    ring_write(t, 0, [1]); t.put(0x10007ffc, 1)
    submit_sram(t, 0, 0x10000, 0, 1, vp=bad, codec=1)
    t.poll(4)
    assert not t.stores and t.result(0) == [2, 1, 0x10000]
    print(image.stem + ': every codec VP, reverse VP DONE, tagged per-engine events, ordered PP, outbox stalls and pool rejection passed', flush=True)


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        check(pathlib.Path(arg))
        check_sram(pathlib.Path(arg))
        check_multi(pathlib.Path(arg))
    if len(sys.argv) < 2:
        raise SystemExit("usage: transport-test.py build/avd-fw-*.bin")
