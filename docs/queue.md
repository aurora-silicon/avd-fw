# Open firmware queue ABI 1 with multi-VP metadata

This MIT protocol carries host-generated VP instruction programs to the CM3.
Firmware selects the VP, configures the frame's hardware FIFO, chains VP to PP,
and publishes tagged completions. It is separate from the Apple firmware ABI.
Firmware retains the legacy interrupt protocol until its first queue doorbell.
A hardware reset and firmware boot are required to leave queue mode.

The original ABI 1 descriptor layout and version stay intact. Multi-VP clients
negotiate the extension described below and set its descriptor flag. Existing
ABI 1 entries remain supported with their caller-supplied FIFO setup and PP kick.
Transport selection stays separate from normalized frame work and VP scheduling.

## Images and pools

| Image | HEVC VP indices | H.264 | VP9 | AV1 | FIFO slots/mask | LSR addresses |
| --- | --- | --- | --- | --- | --- | --- |
| v2-t0 | 0..1 | 2 | 3 | none | 7 | yes |
| v3-t0 | 0..3 | 4..7 | 8 | none | 15 | yes |
| v3-t1 | 0..3 | 4..7 | 8 | none | 15 | yes |
| v3-t2 | 0..3 | 4..7 | 8 | none | 15 | no |
| v4-t0 | 0..3 | 4..7 | 8 | 9..10 | 15 | no |
| v5-t0 | 0..1 | 4 | 8 | 9 | 7 | no |
| v5-t1 | 0..3 | 4..7 | 8 | 9..11 | 15 | no |
| v5-t2 | 0..3 | 4..7 | 8 | 9..11 | 15 | no |

The v5-t2 engine counts are inferred from the full-size family and require
silicon validation. The v3-t2 image selects the existing v3t1 tunables used for
T6020, with non-LSR FIFO addresses. There is no separate public v3t2 tuning
array. Both Make and Meson build all eight images, including v5t2 tunables.

## Discovery

All wire words are little endian. The host reads sixteen u32 words from the DT
SRAM resource at offset `0x7fc0` after boot-ready:

| Word | Value |
| --- | --- |
| 0 | magic `0x41565131`, published last |
| 1 | ABI version 1 |
| 2, 3 | descriptor count 16, stride 64 bytes |
| 4 | SRAM aperture address in the doorbell address space |
| 5, 6 | ring offset `0x8000`, packet payload size 256 words |
| 7 | PIODMA destination header, use verbatim |
| 8 | VP register index limit |
| 9 | codec mask in bits 3:0; bit 31 advertises multi-VP metadata |
| 10 | independent FIFO slots, 7 or 15 |
| 11..15 | reserved for program-source transport extensions |

Codecs are HEVC=0, H.264=1, VP9=2, AV1=3. The VP limit is an address bound,
not an engine count: tier 0 has holes. Missing magic allows legacy operation;
an unknown ABI version must never receive queued descriptors.

If root word 9 has bit 31 set, read the separate 64-byte extension block at
SRAM `+0x7f80`: word 0 magic `0x41564d31` (AVM1), word 1 extension version 1,
words 2..5 one codec pool each (base in bits 7:0, count in bits 15:8), word 6
descriptor flag mask (bit 31 for multi-VP metadata), remaining words zero.
Require both the root capability and extension magic/version. The root bit is
cleared by old firmware at boot, so stale extension SRAM cannot masquerade as
support after an image rollback. Host match data and pool geometry must agree.

CM3 sees SRAM byte zero at `0x10000000`. Private queue state occupies
`+0x6000..+0x7f7f`, extension info `+0x7f80..+0x7fbf`, descriptors `+0x8000..+0x83ff`, and the single staging
buffer `+0x9000..+0x93ff`. The smallest supported SRAM is `0xc000` bytes.
The host must not use these regions for scratch or legacy commands.

| Image | Doorbell SRAM aperture | PIODMA base |
| --- | --- | --- |
| v2-t0 | `0x0108c000` | `0x40070000` |
| v3-t0/t1/t2 | `0x01090000` | `0x40070000` |
| v4-t0 | `0x01092000` | `0x41070000` |
| v5-t0 | `0x01090000` | `0x41070000` |
| v5-t1 | `0x01092000` | `0x41070000` |
| v5-t2 | `0x01094000` | `0x41070000` |

The aperture must match the DT SRAM window. T6040 uses v5-t2; using v5-t1
would stage DMA data into the wrong SRAM region. The host writes the physical
SRAM base shifted right four to its PIODMA resource `+0x24` before CM3 boot.
Both decode and PIODMA DART stream IDs must be attached to its DMA domain.

## Descriptor and ownership

Each descriptor remains 64 bytes (`src/queue.h`):

| Offset | ABI 1 field | Meaning |
| --- | --- | --- |
| 0x00 | magic | `0x41565131` |
| 0x04 | tag | full u32, low nibble equals ring index |
| 0x08 | codec | 0..3 |
| 0x0c | vp_slot | legacy VP hint; extended entries let firmware select the VP |
| 0x10, 0x14 | program_lo/hi | program source address |
| 0x18 | program_words | payload words, 1..0x100000 |
| 0x1c | pp_kick | legacy PP kick; bits 7:4 carry the FIFO index for all entries |
| 0x20 | state | host publishes 1, firmware publishes 2 |
| 0x24 | status | firmware: 0 success, 1 invalid, 2 fatal I/O |
| 0x28 | completion_tag | full submitted tag, unmodified |
| 0x2c | reserved[0] | bit 31 enables multi-VP metadata; low bits remain transport flags |
| 0x30 | reserved[1] | context in bits 9:0, PREV_DONE in bit 31, other bits zero |
| 0x34, 0x38 | reserved[2..3] | frame-owned FIFO DMA address low/high |
| 0x3c | reserved[4] | PP EXEC count, 1..program_words |

Extended entries derive their FIFO index from pp_kick and require it to equal
the ring index and be less than FIFO slots. The maximum is 7 or 15, never 8 or
16. FIFO memory is 256-byte aligned, sized for the instruction stream, and
remains owned until notification consumption. Linux uses its existing 12-MiB
FIFO allocation. LSR images require FIFO addresses below 38 bits.

Without the multi-VP flag, reserved metadata words remain zero and the existing
ABI 1 host configures FIFO registers before submitting. The firmware leaves its
IOVA/cache/xfer setup intact, enables the selected VP's IRQs and uses the supplied
single PP kick exactly. Legacy entries retain overall completion order; the
legacy host is responsible for codec dependencies. Both forms enforce distinct
live FIFO ownership and the per-image FIFO budget.

The scheduler receives normalized work, including opaque program-source flags;
it never interprets PIODMA packets or reads program memory. Only the feed
adapter handles program transport. Low transport flags are rejected until their
source adapter is present; adding SRAM-direct does not require a new scheduler.
The multi-VP root capability and separate info block leave words 11..15 of the
original information block available for that transport's geometry.

Doorbell arrival defines submission order. The host retains descriptors, packet
buffers and FIFO mappings unchanged until it consumes the matching completion.
Publishing state=2 alone does not release ownership; the notification can still
be pending. No tag/index can be recycled while an old notification remains.

Every VP EXEC in the supplied program must contain the FIFO index encoded
in pp_kick. Extended clients use the ring index. Firmware copies VP programs unchanged, including start, continuation
and final END words. It does not scan arbitrary operands for opcode patterns.
The host writes/synchronizes the packet buffer, writes the complete descriptor
through SRAM I/O accessors, issues a write barrier and state readback, then
writes exactly `info[4] + 0x8000 + 64 * index` to AP mailbox 0 TX (`+0x54`).
Check mailbox 0 EMPTY first; a busy inbox owns nothing and is retried later.

## Dispatch and dependencies

A free engine takes the oldest ready entry for its codec. Occupied pools and
unsatisfied dependencies are skipped so another codec/context can progress.
Selection rotates within each pool; one VP owns at most one PUSH/VP frame.
After VP DONE that engine is free for another frame, while the old frame keeps
its distinct FIFO until PP and completion finish.

Dependency 1 waits until every earlier accepted frame of that context has
completed PP. VP DONE alone is insufficient. Other contexts can overlap.
The host also enforces dependencies before submission. Linux defaults to
`AVD_DEP_PREV_DONE`, including H.264 and HEVC: same-stream VP independence and
reference/scratch-state safety have not been proven. Codecs may explicitly
opt into NONE only with that proof and independent per-frame scratch.

The single SRAM staging buffer stays with a partially fed program. DMA polling
is bounded; FIFO pressure does not block IRQ handlers. On a full VP queue,
firmware arms status bit 3 as a not-full interrupt, rechecks occupancy to close
the drain-before-unmask race, and sleeps. The handler masks/acknowledges FULL
and wakes the feeder. FULL may remain level-asserted while masked: its ACK is
flushed once without waiting for the bit to clear. No occupancy reads occur
while waiting for that IRQ.
This register/IRQ behavior still requires validation on each generation.

## PIODMA packets

Allocate `ceil(program_words / 256) * 1028` bytes. Each packet contains the
advertised destination header followed by up to 256 payload words. Zero-pad
the final unused payload words. Packet stride is always 1028 bytes.

Firmware writes the source IOVA low/high at PIODMA `+0x4c/+0x50`, clears stale
DONE at `+0x04`, then writes `((payload_words + 1) << 8) | 0x11` to `+0x54`.
It waits with IRQs enabled for DONE bit 0 and acknowledges it before using the
staged words. The header's destination mechanism and transfer byte units follow
the public MIT emulator. The header must be trusted kernel input: hardware
uses it before firmware can inspect the transferred data.

## PP and completions

PP is serialized across codecs and contexts. VP DONE queues that frame for PP.
When PP is idle firmware writes:

```
EXEC | REV | FIFO_IDX(fifo) | FIFO_MASK | codec_mask
```

It then writes `num_segments - 1` continuation EXECs with the same index, mask
and codec bits and no REV. REV is bit 8 on v2, bit 9 on later images; the codec
mask is `0xfff000` only for v2 VP9. PP DONE completes exactly the active frame.

Completed frames publish in submission order per context. A completed context
can bypass another context's unfinished work. Firmware writes status and the
full completion tag, DMB, state=2, DMB, then posts:

```
0x54530000 | (tag & 0x0000fff0) | ring_index
```

Outbox pressure never drops or overwrites a completion. The host requires the
exact prefix, short tag, full tag and state=2 before calling its completion
handler. It retains its own per-context retirement ordering as well.

Decode errors, impossible events and PIODMA timeouts halt the shared pipeline.
Accepted unfinished entries complete with status 2. Duplicate/out-of-capacity
doorbells also emit `0x5452ffff` after accepted completions drain. These errors
are not DMA quiescence: the host must reset the entire decoder, stop DMA, drain
mailboxes and fail outstanding frames before unmapping any buffers. A per-frame
watchdog remains mandatory. Abort/streamoff resets the shared queue.

## Verification and provenance

```
make test
sh tests/build-all.sh
python3 tests/check-images.py build/avd-fw-*.bin
python3 tests/transport-test.py build/avd-fw-*.bin
meson setup build/meson --cross-file llvm.ini --buildtype=release
meson compile -C build/meson
meson test -C build/meson --print-errorlogs
```

The transport test requires Unicorn and llvm-nm. It runs actual CM3 machine
code with synthetic MMIO/PIODMA for all eight images. Tests cover concurrent
contexts on VP0/VP1, all codec pools, FIFO bounds, predecessor dependencies,
serialized multi-segment PP, out-of-order events, ordered tags, outbox pressure,
FULL wakeup without polling, fatal errors and reset. They prove software
behavior, not silicon decoding or throughput.

Public implementation references are the MIT
[CM3 submission and VP-to-PP firmware](https://github.com/sofus13/avd-fw/tree/avd-next-fw)
and [AVD emulator](https://github.com/eiln/avd/blob/main/avd_emu.py).
Existing repository authorship and MIT notices are retained. No Apple firmware,
private implementation source or captured payload is added by this change.
