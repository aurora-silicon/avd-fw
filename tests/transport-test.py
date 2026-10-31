#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run built CM3 transport code against a synthetic MMIO/PIODMA model.

Requires the optional `unicorn` Python package and llvm-nm on PATH.
This checks software integration, not silicon register semantics.
"""
import pathlib
from functools import cache
import struct
import subprocess
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC


@cache
def image_data(image):
    elf = image.with_suffix(".elf")
    if not elf.exists():
        elf = image.parent.parent / elf.name
    nm = subprocess.check_output(["llvm-nm", str(elf)], text=True)
    symbols = {name: int(addr, 16) for addr, kind, name in
               (line.split() for line in nm.splitlines() if len(line.split()) == 3)}
    return image.read_bytes(), symbols


class Transport:
    def __init__(self, image):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.uc.mem_map(0, 0x20000)
        self.uc.mem_map(0x10000000, 0x20000)
        self.uc.mem_map(0x40000000, 0x200000)
        self.uc.mem_map(0x41000000, 0x200000)
        self.uc.mem_map(0x50010000, 0x4000)
        self.uc.mem_map(0xe000e000, 0x2000)
        data, self.symbol = image_data(image)
        self.uc.mem_write(0, data)
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
        self.busy, self.full, self.dma_ticks, self.dma_timeout = False, False, 0, False
        self.clear = set()
        self.level_reads = 0
        self.uc.hook_add(UC_HOOK_MEM_READ, self.read)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.write)
        self.call("avdq_fw_init")
        self.info = [self.u32(0x10007fc0 + i * 4) for i in range(16)]
        assert self.info[:4] == [0x41565131, 1, 16, 64]
        expected = 0x0108c000 if self.version == 2 else 0x01090000 if self.version == 3 or (self.version == 5 and self.tier == 0) else 0x01094000 if (self.version == 5 and self.tier == 2) else 0x01092000
        assert self.info[4] == expected and self.info[8] == self.vps
        assert self.info[10] == (7 if self.version == 2 or (self.version == 5 and self.tier == 0) else 15)
        self.multi = [self.u32(0x10007f80 + i * 4) for i in range(7)]
        assert self.info[9] & 0x80000000
        assert self.multi[:2] == [0x41564d31, 1] and self.multi[6] == 0x80000000
        assert self.info[11:] == [0] * 5  # Reserved for transport extensions.
        self.ring = self.info[4] + self.info[5]

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
            self.put(addr, 0 if self.busy else 4)
        if self.decode + self.maximum <= addr < self.decode + self.maximum + self.vps * 4:
            self.put(addr, 256)
        if self.decode + self.current <= addr < self.decode + self.current + self.vps * 4:
            self.level_reads += 1
            self.put(addr, 256 if self.full else 0)
        if addr == self.dma + 4:
            if self.dma_timeout:
                self.put(addr, 0)
            elif self.dma_ticks:
                self.dma_ticks -= 1
                self.put(addr, int(self.dma_ticks == 0))

    def write(self, uc, access, addr, size, value, user):
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
            self.stores.append(((addr - self.decode - self.fifo) // 4, value))
        if addr == self.decode + self.pp:
            self.kicks.append(value)
        if addr == 0x50010060:
            assert not self.busy, "unread notification overwritten"
            self.busy = True
            self.notifications.append(value)
        # DONE/error are W1C. FULL may stay level-asserted even after its ACK.
        status = self.decode + (0x4060 if self.version == 2 else 0x124 if self.version == 3 else 0x194)
        if (addr == status if self.version == 2 else status <= addr <= status + self.pp_idx * 4):
            full_mask = sum(8 << (vp * 5) for vp in range(4)) if self.version == 2 else 8
            if value & full_mask:
                self.clear.discard(addr)
            else:
                self.clear.add(addr)

    def submit(self, idx, tag, words, codec=0, context=0, dep=0, segments=1, extended=True):
        source = 0x100004000 + idx * 0x10000
        for p in range((len(words) + 255) // 256):
            self.packets[source + p * 1028] = (self.info[7], words[p * 256:(p + 1) * 256])
        base = self.multi[2 + codec] & 0xff
        reserved = [0x80000000, context | (dep << 31),
                    0x1000000 + idx * 0x1000000, 2, segments] if extended else [0] * 5
        desc = [0x41565131, tag, codec, base, source & 0xffffffff, source >> 32,
                len(words), 0x2b000400 | (idx << 4), 1, 0, 0, *reserved]
        self.uc.mem_write(0x10008000 + idx * 64, struct.pack("<16I", *desc))
        # Exercise the actual mailbox IRQ wrapper and interrupt acknowledgement.
        self.put(0x50010058, self.ring + idx * 64)
        self.call("irq1")

    def poll(self, n=1):
        for _ in range(n):
            self.call("avdq_fw_poll")

    def vp_done(self, vp=0):
        self.call(f"irq{20 + vp * 5 if self.version == 2 else 80 + vp * 5}")

    def not_full(self, vp=0):
        self.call(f"irq{21 + vp * 5 if self.version == 2 else 81 + vp * 5}")

    def pp_done(self):
        self.call("irq40" if self.version == 2 else "irq64")

    def idle_sleeps(self):
        saw_wfi = []

        def inspect(uc, addr, size, user):
            if bytes(uc.mem_read(addr, 2)) == b"\x30\xbf":
                saw_wfi.append(True)
                uc.emu_stop()

        hook = self.uc.hook_add(UC_HOOK_CODE, inspect)
        try:
            self.call("avdq_fw_idle")
        except AssertionError:
            if not saw_wfi:
                raise
        finally:
            self.uc.hook_del(hook)
        return bool(saw_wfi)

    def result(self, idx):
        return [self.u32(0x10008000 + idx * 64 + off) for off in (0x20, 0x24, 0x28)]


def check(image):
    t = Transport(image)
    # Distinct contexts occupy both HEVC engines, including tier 0.
    words = list(range(513))
    t.submit(0, 0xabcd0000, words, context=10, dep=1, segments=3)
    t.submit(1, 0x12340001, [0xfeed], context=20, dep=1)
    t.full = True
    t.poll(20)
    reads = t.level_reads
    t.poll(20)
    assert not t.stores and len(t.fetches) == 1 and t.level_reads == reads
    # Draining alone does not cause software to busy poll the VP queue.
    t.full = False
    t.poll(10)
    assert not t.stores
    t.not_full()
    t.poll(550)
    assert t.stores == [(0, w) for w in words] + [(1, 0xfeed)]
    assert len(t.fetches) == 4 and not t.kicks and not t.notifications
    # Distinct 64-bit FIFO mappings, including v3-t2's non-LSR register form.
    lo = 0x4068 if t.version == 2 else 0x150 if t.version == 3 else 0x1d0
    hi = 0x30c if t.version == 3 else 0x20c
    lsr = t.version == 2 or (t.version == 3 and t.tier < 2)
    for idx in (0, 1):
        addr = 0x200000000 + 0x1000000 + idx * 0x1000000
        assert t.u32(t.decode + lo + idx * 4) == (addr >> 8 if lsr else addr & 0xffffffff)
        if not lsr:
            assert t.u32(t.decode + hi + idx * 4) == addr >> 32

    start = 0x100 if t.version == 2 else 0x200
    mask = t.info[10]
    # VP1 finishes first. Context20 completion bypasses context10.
    t.vp_done(1); t.poll()
    assert t.kicks == [0x2b000010 | start | mask]
    t.vp_done(0); t.poll()
    assert len(t.kicks) == 1
    t.pp_done(); t.poll()
    assert t.result(1) == [2, 0, 0x12340001]
    assert t.notifications == [0x54530001]
    assert t.kicks[1:] == [0x2b000000 | start | mask, 0x2b000000 | mask, 0x2b000000 | mask]
    t.pp_done(); t.poll(5)
    assert t.result(0)[0] == 1 and len(t.notifications) == 1
    t.busy = False; t.poll()
    assert t.result(0) == [2, 0, 0xabcd0000]
    assert t.notifications[-1] == 0x54530000
    # Same-context predecessor must finish PP before the next VP starts.
    t = Transport(image)
    t.submit(0, 0x20000, [1], context=10)
    t.submit(1, 0x20001, [2], context=10, dep=1)
    t.submit(2, 0x20002, [3], context=20, dep=1)
    t.poll(30)
    assert t.stores == [(0, 1), (1, 3)]
    t.vp_done(0); t.poll()
    assert len(t.stores) == 2
    t.pp_done(); t.poll(12)
    assert t.stores[-1][1] == 2
    # Out-of-order completion within a context still publishes ordered tags.
    t = Transport(image)
    t.submit(0, 0x30000, [1], context=10)
    t.submit(1, 0x30001, [2], context=10)
    t.poll(20); t.vp_done(1); t.poll(); t.pp_done(); t.poll()
    assert not t.notifications
    t.vp_done(0); t.poll(); t.pp_done(); t.poll()
    assert t.result(0) == [2, 0, 0x30000]
    t.busy = False; t.poll()
    assert t.result(1) == [2, 0, 0x30001]
    # Reject indices outside the independent FIFO budget without DMA stores.
    t = Transport(image)
    t.submit(t.info[10], 0x40000 | t.info[10], [1]); t.poll(8)
    assert not t.stores and not t.fetches
    assert t.result(t.info[10]) == [2, 1, 0x40000 | t.info[10]]
    # Decode errors halt all accepted work; late events cannot kick PP.
    t = Transport(image)
    t.submit(0, 0x50000, [1]); t.submit(1, 0x50001, [2]); t.poll(20)
    t.call("irq19" if t.version == 2 else "irq79")
    t.poll(); t.busy = False; t.poll()
    assert t.result(0) == [2, 2, 0x50000] and t.result(1) == [2, 2, 0x50001]
    t.vp_done(); t.poll()
    assert not t.kicks
    # Duplicate doorbells preserve the tag and emit a separate fatal token.
    t = Transport(image)
    t.submit(0, 0x60000, [1]); t.poll(8)
    t.put(0x50010058, t.ring); t.call("irq1"); t.poll(2)
    assert t.result(0) == [2, 2, 0x60000]
    t.busy = False; t.poll()
    assert t.notifications[-1] == 0x5452ffff
    # Dirty SRAM reset plus 64-bit program IOVA and a high generation tag.
    t.call("avdq_fw_init")
    t.busy = False; t.notifications.clear()
    t.submit(0, 0xfffffff0, [3]); t.poll(8)
    t.vp_done(); t.poll(); t.pp_done(); t.poll()
    assert t.result(0) == [2, 0, 0xfffffff0]
    t.poll()
    t.not_full()  # A late/pending FULL without a feeder must not keep CM3 awake.
    assert t.idle_sleeps()
    # PP work counts are full u32 values, including a 256-tile frame.
    t = Transport(image)
    t.submit(0, 0x90000, [1] * 256, segments=256); t.poll(300)
    t.vp_done(); t.poll()
    assert len(t.kicks) == 256
    assert t.kicks[0] == 0x2b000000 | start | t.info[10]
    assert t.kicks[-1] == 0x2b000000 | t.info[10]
    t.pp_done(); t.poll()
    assert t.result(0) == [2, 0, 0x90000]
    # An original ABI 1 entry has no metadata flag or FIFO IOVA extension.
    t = Transport(image)
    lo = 0x4068 if t.version == 2 else 0x150 if t.version == 3 else 0x1d0
    t.put(t.decode + lo, 0xabc000)
    t.submit(0, 0x80000, [7], extended=False); t.poll(10)
    assert t.u32(t.decode + lo) == 0xabc000
    t.vp_done(); t.poll()
    assert t.kicks == [0x2b000400]  # Legacy caller-supplied PP kick stays exact.
    t.pp_done(); t.poll()
    assert t.result(0) == [2, 0, 0x80000]
    # Exercise actual IRQ wrappers for every advertised engine of every codec.
    for codec in range(4):
        t = Transport(image)
        base, count = t.multi[2 + codec] & 0xff, t.multi[2 + codec] >> 8
        for idx in range(count):
            t.submit(idx, 0x70000 | idx, [0x100 + idx], codec=codec, context=idx)
        t.poll(80)
        assert t.stores == [(base + idx, 0x100 + idx) for idx in range(count)]
        for idx in reversed(range(count)):
            t.vp_done(base + idx); t.poll()
            expected = 0x2b000000 | (idx << 4) | t.info[10] | start
            if codec == 2 and t.version == 2:
                expected |= 0xfff000
            assert t.kicks[-1] == expected
            t.pp_done(); t.busy = False; t.poll()
            assert t.result(idx) == [2, 0, 0x70000 | idx]
    print(image.stem + ": concurrent VP0/VP1 contexts, FIFO cap, dependencies, ordered tags, PP continuations, FULL IRQ, error/reset passed", flush=True)


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        check(pathlib.Path(arg))
    if len(sys.argv) < 2:
        raise SystemExit("usage: transport-test.py build/avd-fw-*.bin")
