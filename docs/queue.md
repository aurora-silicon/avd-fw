# Open firmware frame queue, ABI 2

This is the MIT open backend protocol, not an Apple firmware ABI. All words are
little endian. It preserves legacy `0x100 | vp` / `0x1000` interrupt messages
until the first queue descriptor doorbell after boot. The v5 Tansy diagnostic
mailbox commands also remain available in legacy mode. Never mix the two modes;
a hardware reset and firmware boot are required to switch back to legacy mode.
No Apple firmware image, extracted table, or captured payload is needed.

## Memory and discovery

The host maps the DT `sram` resource (the command aperture). CM3 sees its byte
zero at `0x10000000`. Offsets below are relative to that resource, **not** to the
`code`, `ctrl`, or mailbox resource. Firmware publishes this read-only 64-byte
information block at SRAM `+0x7fc0` before raising the existing boot-ready flag:

| Offset | u32 value |
| --- | --- |
| 0x00 | magic `0x41565131` (`AVQ1`), published last |
| 0x04 | ABI version, 2 (ABI 1 plus the SRAM ring and multi-kick PP) |
| 0x08 | descriptor count, 16 |
| 0x0c | descriptor stride, 64 |
| 0x10 | SRAM aperture address in the four-byte doorbell address space |
| 0x14 | descriptor ring offset, `0x8000` |
| 0x18 | maximum payload words per program packet, 256 |
| 0x1c | PIODMA destination header for every packet (use verbatim) |
| 0x20 | VP register index limit (exclusive), 4 / 9 / 12 |
| 0x24 | codec mask: bits HEVC=0, H.264=1, VP9=2, AV1=3 |
| 0x28 | hardware FIFO slots: cap frames in flight at min(16, this) |
| 0x2c | features: bit 0 SRAM ring, bit 1 multi-kick PP, bit 2 VP DONE, bit 3 VP pools |
| 0x30 | SRAM ring offset, `0x0` |
| 0x34 | SRAM ring size in words, `0x1800` (24 KiB) |
| 0x38 | firmware: SRAM ring words consumed (free-running, live) |
| 0x3c | host: SRAM ring words written (free-running, host-owned) |

Read the block after boot-ready. Missing magic means old firmware: use the
legacy backend, or reject queue mode. An unknown version must not be submitted.
Check count/stride/packet size and the actual SRAM resource length. Use the
advertised address instead of deriving it from the host physical address.

The ring occupies `+0x8000..+0x83ff`. Firmware private state is at
`+0x6000..+0x7f7f`, and its 1024-byte PIODMA staging buffer is at `+0x9000`.
The minimum SRAM size supported by the existing images is `0xc000`. The stack
stays at the existing per-image SRAM end. Do not use these regions for codec
scratch or legacy commands in queue mode.

Current firmware aperture addresses are:

| Image | SRAM address | PIODMA CM3 base | VP FIFO / PP FIFO offset from decode base |
| --- | --- | --- | --- |
| v2-t0 | `0x0108c000` | `0x40070000` | `0x4004 + 4*vp` / `0x4014` |
| v3-t0, v3-t1, v3-t2 | `0x01090000` | `0x40070000` | `0x000c + 4*vp` / `0x0030` |
| v4-t0 | `0x01092000` | `0x41070000` | `0x000c + 4*vp` / `0x0040` |
| v5-t0 | `0x01090000` | `0x41070000` | `0x000c + 4*vp` / `0x0040` |
| v5-t1 | `0x01092000` | `0x41070000` | `0x000c + 4*vp` / `0x0040` |
| v5-t2 (T6040/T6041) | `0x01094000` | `0x41070000` | `0x000c + 4*vp` / `0x0040` |

The aperture must match the SoC's DT `sram` offset from the AVD base
(+0x90000 on T6000, T8112 and T8140; +0x92000 on T8132; +0x94000 on T6040),
because the PIODMA destination header is an address in that space. T6040 must
load `avd-fw-v5-t2` for queue mode: `avd-fw-v5-t1` would place staged words
0x2000 bytes low, inside the firmware's private state. Legacy mode does not
use the aperture, so T6040 legacy decode is unaffected by the image choice.

Firmware image version numbers differ from Linux's hardware revision numbering:
T8103 uses v2, T6000/T8112 use v3, T8122 uses v4, and T8140/T6040 use v5.
The host must select the correct tier and existing match-data VP/codec mapping;
the VP limit is an address bound, not permission to use absent codec slots.

## Descriptor and ownership

Each descriptor is exactly 16 u32 words (`struct avdq_desc` in `src/queue.h`):

| Offset | Field | Owner / meaning |
| --- | --- | --- |
| 0x00 | magic | host: `0x41565131` |
| 0x04 | tag | host: full u32 `(sequence << 4) | ring_index` |
| 0x08 | codec | host: HEVC=0, H.264=1, VP9=2, AV1=3 |
| 0x0c | vp_slot | host: hardware VP index from the advertised pool or SoC match data |
| 0x10 | program_lo | host: low 32 bits of packet-buffer PIODMA IOVA |
| 0x14 | program_hi | host: high 32 bits of that IOVA |
| 0x18 | program_words | host: instruction words, excludes packet headers/padding |
| 0x1c | pp_kick | host: exact single PP FIFO execution command |
| 0x20 | state | host publishes 1 (SUBMITTED); firmware publishes 2 (COMPLETE) |
| 0x24 | status | firmware: 0 success, 1 invalid descriptor, 2 fatal pipeline I/O |
| 0x28 | completion_tag | firmware: full unmodified 32-bit submitted tag |
| 0x2c | flags | host: bit 0 = program is in the SRAM ring (ABI 2) |
| 0x30 | pp_extra | host: continuation PP kicks after pp_kick, 0..255 (ABI 2) |
| 0x34 | pp_cont | host: continuation PP kick word, nonzero if pp_extra (ABI 2) |
| 0x38..0x3c | reserved[2] | host: zero |

The host can choose any free index; doorbell arrival defines submission order.
There is no shared producer counter or sentinel entry: all 16 entries can be
owned by firmware. Firmware maintains a bounded internal FIFO of descriptor
indices and copies each descriptor before processing. The host must not modify
an entry or its program/scratch mappings from its doorbell until consuming its
completion notification. Do not poll state=COMPLETE as permission to reuse the
entry: the mailbox word may still be pending. Keep a saved full tag per index.

The host allocates distinct hardware instruction/FIFO storage for every frame
that may overlap, configures its FIFO IOVA/cache/xfer and VP/PP IRQ masks using
existing SoC match data **before** publishing, and embeds that FIFO index in its
VP instructions and PP command. Those mappings live through PP completion.
The effective in-flight limit is the minimum of 16 and the SoC's available
independent FIFO/context storage. Do not overwrite another live frame's FIFO
registers. Codec dependency barriers (VP9 probability updates, AV1 CDF updates,
references, etc.) remain host responsibilities. A codec needing several PP
commands must combine/split its work into supported one-kick submissions; ABI 1
does not represent a multi-kick PP program.

Submission sequence:

1. Reserve an unused ring index and full tag; construct the complete VP program,
   including the existing start/continuation/final END words. Firmware copies
   these words unchanged; it does not infer codec instructions or insert END.
2. Prepare and DMA-sync the packet buffer in the **PIODMA** address space. This
   is the mapper for PIODMA source fetches (mapper 1 where separate); it is not
   automatically the VP's instruction/data mapper. The host must arrange the
   appropriate DART mappings, including on platforms whose current DT only
   exposes the primary mapping. An arbitrary CPU pointer is never an IOVA.
3. Write the descriptor with status/completion_tag/reserved zero and state=1.
   Use `memcpy_toio`/`writel` for the SRAM aperture and the proper DMA ownership
   operations for the packet buffer. Issue `wmb()` and read back the descriptor
   state to flush posted aperture writes before ringing the mailbox.
4. Wait for mailbox 0's TX-full condition to clear (nonblocking `-EBUSY` is fine).
   Write exactly one u32: `info.sram_address + 0x8000 + 64 * index`, through the
   existing AP-to-CM3 mailbox. Never post the same descriptor twice.

Reject/retain locally when no descriptor/FIFO/mailbox capacity is available.
The firmware doorbell ISR only puts the index into its 16-entry inbox and
acknowledges the interrupt. Descriptor copies, DMA polling, and scheduling happen
in the foreground so slow fetches/FIFO pressure cannot block completion IRQs.

## SRAM ring (ABI 2)

PIODMA fetches need the decoder's second DART stream, which the device trees
in use do not describe yet. ABI 2 adds a mode that needs no DMA at all: the
host streams each program into a word ring in CM3 SRAM and the firmware copies
it to the VP FIFO from there.

* The ring is SRAM `+0x0000..+0x5fff`, 0x1800 words. Queue mode never runs the
  Tansy diagnostic commands, so nothing else uses that range; firmware never
  writes it.
* Two free-running u32 word counters: the host's write count at info `+0x3c`
  and the firmware's read count at info `+0x38`. Word `n` lives at ring word
  `n % 0x1800`. Both start at 0 after every firmware boot.
* A ring descriptor sets `flags` bit 0, `program_hi` = 0 and `program_lo` = the
  write count of its first word. Firmware faults the queue (status 2) if that
  does not equal its read count when the frame starts, so programs are consumed
  in exactly doorbell order. Submit descriptors in the same order their words
  were placed.
* The host may ring the doorbell before the whole program is in the ring and
  keep streaming: it may run at most 0x1800 words ahead of the read count, and
  must publish the write count (after `wmb()`) as it goes. Programs of any
  length up to `AVDQ_MAX_WORDS` work this way. The firmware copies at most 64
  words per scheduler step, stops at the host's write count and at the VP FIFO
  capacity, and waits otherwise; the host's per-frame watchdog covers a stall.
* PIODMA descriptors (`flags` = 0) keep the ABI 1 rules. Do not mix the two in
  one session.

Multi-kick PP: codecs that issue one PP EXEC per tile or segment set `pp_extra`
to the number of continuation kicks and `pp_cont` to that word. Firmware writes
`pp_kick` then `pp_extra` copies of `pp_cont`, the same sequence the legacy host
wrote after VP DONE. A frame completes on its first PP DONE; further PP DONEs
while no frame is in PP are counted and ignored, as the legacy host did.

## Program packets and PIODMA

`program_words` is in `[1, 0x100000]`. The IOVA is nonzero, four-byte aligned,
and must leave space for all packets without wrapping u64. Allocate
`ceil(program_words / 256) * 1028` bytes. Every packet has fixed stride 1028:

```
packet[0]       = info.packet_header;      // same fixed destination each time
packet[1..256]  = up to 256 instruction words; zero-pad final unused words
```

For packet p, firmware fetches from `program_iova + p * 1028`. It writes low/high
IOVA at PIODMA `+0x4c/+0x50` and starts command `((payload_words + 1) << 8) | 0x11`
at `+0x54`. Command bits 31:6 encode the transfer size in **bytes**, including
the four-byte packet header (payload words plus one, times four). For comparison, the existing
Tansy `0x5c11` command fetches 368 bytes: a header plus 91 continuation words.
It clears stale DONE at `+0x04` before issuing, polls DONE bit 0 without masking interrupts, then
acknowledges DONE. Polling has a bounded software iteration timeout.

Open hardware question: public notes describe the per-segment header as
`SIZE_words << 18 | DEST | BIT(0)`, while this ABI advertises a header with
the size field zero and carries the size only in the command register. If
device bring-up sees PIODMA timeouts, OR `payload_words << 18` into each
packet header and record which form the engine accepts.

The engine consumes the destination header and places the payload into SRAM
`+0x9000`. CM3 writes those words to the selected VP instruction FIFO, using
local stores with conservative spacing. It pauses when the existing per-VP FIFO
occupancy reaches its capacity and resumes when it drains. Only one packet
buffer is active at once; all words of a frame are submitted before the next
frame's program. Multiple complete programs can await VP DONE in the same VP
FIFO, and different VPs can finish independently. No host per-word MMIO or host
VP-to-PP interrupt round trip is involved.

The destination header is trusted host input: it **must** equal the advertised
header. The PIODMA engine follows it before CM3 can inspect the result. This ABI
is for a trusted kernel backend, not an untrusted DMA command interface. There
is no guessed direct-I/O-virtual-address load by CM3 and no guessed PIODMA
FIFO-destination mode: SRAM-to-FIFO CM3 word copies are deliberate.

## VP, PP, and completion

A VP DONE is associated with the oldest fully submitted frame for that VP.
Firmware queues it for PP and writes its supplied PP kick when PP is idle.
Exactly one PP frame is active at a time. PP DONE finishes that frame. It never
sends the legacy VP/PP messages while queue mode is active. Tagged completions
retire in overall submission order, even if different VPs finish out of order.

The firmware writes status and the **full** completion_tag, issues a DMB, writes
state=2, issues a DMB, then posts one four-byte outbox word:

```
0x54530000 | (tag & 0x0000fff0) | ring_index
```

Bits 15:4 carry the low 12 sequence bits; bits 3:0 select the record. All u32 tags
are supported, including `0xffffffff`. The host must test the exact high 16-bit
prefix before interpreting legacy status bits; `0x5453xxxx` also has some of
those bits set. After reading the notification, read the descriptor through I/O
accessors, require state=2, require the short sequence and saved **full** tag to
match completion_tag, then call `avd_frame_complete(avd, full_tag, status)`.
Translate statuses 1/2 to `-EINVAL`/`-EIO`. Drain outbox words until the mailbox
reports empty, acknowledging each using the existing mailbox convention. Only
then release/reuse that notified index and its successful frame's mappings.

Firmware checks outbox EMPTY before publishing; unread notifications are never
overwritten. A blocked host can stall completion retirement without losing tags
or overrunning the 16-frame array. The host must drain stale mailbox data before
boot/reset and must never reuse a tag/index while its old notification remains.
These ownership rules, not only the 12-bit short sequence, prevent ABA reuse.

Invalid descriptors complete with status 1 without executing any instructions.
An impossible/duplicate VP or PP event, decode error, or PIODMA timeout halts new
work and completes all accepted unfinished frames with status 2. Those error
notifications are **not** proof that hardware/DMA has quiesced: the host must
reset the whole decoder, stop DMA, drain mailboxes, and fail every outstanding
frame (including posted descriptors not yet accepted) before unmapping/reusing
any buffers. A duplicate/out-of-capacity doorbell additionally produces
`0x5452ffff` after accepted completions drain. This has a different prefix from
all valid tagged completions. The host must apply the same whole-queue reset.
Ignore stale notifications across reset generations. Legacy exception/debug mailbox words also require a whole-queue reset in queue
mode. A per-frame host watchdog
is mandatory for stalled VP/FIFO/PP and for a DMA/DART fault that never completes.
A streamoff/abort/suspend drains or resets the entire shared queue; ABI 1 has no
per-context cancellation message. Reboot/reload explicitly initializes the
private scheduler, rather than relying on SRAM or BSS being zero.

## Provenance and verification boundary

The register layout and legacy IRQ path come from this MIT repository and the
MIT AVD driver in [aurora-silicon/linux](https://github.com/aurora-silicon/linux/tree/aurora-wip/drivers/media/platform/apple/avd).
The PIODMA packet destination/size mechanism and `0x40070000` base are public in
[Eileen Yoon's MIT emulator](https://github.com/eiln/avd/blob/main/avd_emu.py).
The `0x41070000` base and low/high address/command/DONE sequence already exist
in this repository's v5 Tansy transport (`src/irq.c`, commit `9cbde5c`).
The v4 PIODMA base is an **inference** from the decode/PIODMA register-bank
relocation shared with v5; it has not been measured here. Only v5 currently has
explicit PIODMA boot tunables in this repository. Older engines retain their
existing platform initialization. SRAM aperture addresses follow the existing
public DT resource sizes and CM3 aliases.

Builds and host tests establish layout and scheduler behavior, not silicon
correctness or macOS-like throughput. New queued DMA packet headers, older
PIODMA initialization/mappings, FIFO capacity behavior, queued END/DONE event
accounting, per-frame context isolation, and output correctness require the
lead's device tests. No hardware was requested for this change.

## Reproduce builds and host tests

Requirements: Clang with the ARM bare-metal target, LLD, llvm-objcopy, a native C
compiler, Meson >=1.4, Ninja. Keep LLVM/LLD tools on PATH.

The Makefile and Meson (with `llvm.ini` and its default options) build the same
images byte for byte. Do not pass `--buildtype` or `-Doptimization` to Meson,
and do not let a distribution inject its own CFLAGS: either changes the code.
`arm-none-eabi-gcc.ini` is a GCC alternative and produces different images.

```sh
make test
sh tests/build-all.sh
python3 tests/check-images.py build/avd-fw-*.bin
meson setup build/meson --cross-file llvm.ini
meson compile -C build/meson
meson test -C build/meson --print-errorlogs
sh tests/check-reproducible.sh   # Make and Meson images must be byte-identical

# Optional integration test of actual ARM machine code, with synthetic MMIO:
python3 -m venv build/test-venv
build/test-venv/bin/pip install unicorn
build/test-venv/bin/python tests/transport-test.py build/avd-fw-*.bin
```

For the Linux lead, after implementing this ABI and before device qualification:

```sh
make O=out ARCH=arm64 LLVM=1 defconfig
make O=out ARCH=arm64 LLVM=1 asahi.config
make O=out ARCH=arm64 LLVM=1 -j8 Image modules dtbs
```

Run the firmware build/test commands above in the firmware checkout. Kernel
builds use a controller build slot. Device runs must use controller-approved
requests with the explicit device identity; install the matching image through
the lead's normal reversible firmware packaging, retaining the previous image.
Validate legacy single-frame decode first, then queue depth 1/2/4 and the safe
per-SoC maximum, compare framemd5 against software decoding for all supported
codecs/bit depths, and exercise streamoff, watchdog/reset, and tag wrap. Record
throughput separately; no performance result is claimed by this implementation.

For a patched FFmpeg providing the `v4l2request` hwaccel, these are the exact
comparison commands (set `clip` to each local test stream and `fmt` to `nv12`
for 8-bit or `p010le` for 10-bit output supported by that driver):

```sh
ffmpeg -v error -hwaccel v4l2request -hwaccel_output_format drm_prime \
  -i "$clip" -map 0:v:0 -an -sn -dn -vf "hwdownload,format=$fmt" \
  -fps_mode passthrough -f framemd5 -y hw.framemd5
ffmpeg -v error -i "$clip" -map 0:v:0 -an -sn -dn -pix_fmt "$fmt" \
  -fps_mode passthrough -f framemd5 -y sw.framemd5
diff -u sw.framemd5 hw.framemd5
```

These commands are proposed device validation, not results from this change.
The backend/codec prepare path must expose the supported download pixel format;
if the actual driver uses a different format, use that same format on both runs
and record the difference. Start at queue depth 1, then increase without changing
the input or output format. Use the existing core's trace/debug instrumentation
to record maximum in-flight count and tag correlation; ABI 1 adds no queue-depth
module parameter. To measure steady-state decode independently of output disk
I/O, repeat the hardware decode with `-benchmark -f null -` as the output.

### VP picture ownership

A fully fed program still owns its VP until that VP's DONE event. FIFO capacity
only permits more words; it does not release the VP's live picture state.
The foreground scheduler therefore leaves a successor pending when an earlier
frame on the same VP is in `AVDQ_VP`. After DONE it can feed the successor while
the earlier picture is PP-ready or running PP. Independent VPs can still run
concurrently. A blocked program cannot be skipped because the SRAM word ring is
in global submission order. The host watchdog continues to bound a missing
DONE; errors halt all accepted work as before.

## Optional codec VP pools (ABI 2 feature bit 3)

`avd/multi-vp-2` retains the ABI 2 SRAM ring and tagged VP DONE feature from
`bf49a31b`. It ports the per-image codec pools from the earlier MIT multi-VP
implementation without changing descriptor offsets or restoring its older
transport. No AVM1 descriptor extension is accepted or required.

When info word 11 has bit 3, SRAM +0x7f80 contains eight little-endian u32 words:

| Word | Meaning |
| --- | --- |
| 0 | `0x41564d32` (AVM2), published before the main info magic |
| 1 | Pool record version, 1 |
| 2..5 | HEVC, H.264, VP9, AV1 masks; bit n names hardware VP n |
| 6 | Bit n means codec n's pool count is inferred |
| 7 | Reserved, zero |

The masks must be disjoint, fit the VP register limit in info word 8 and agree
with info word 9's codec mask. Firmware validates every descriptor's codec/VP
pair against the same pool used to publish the record, including tier-0 holes.
Older hosts continue to submit the first VP of each codec with unchanged ABI 2
descriptors. New hosts use one VP per codec unless feature bit 3 and a valid
pool record are both present.

| Image | HEVC VPs | H.264 VPs | VP9 VPs | AV1 VPs |
| --- | --- | --- | --- | --- |
| v2-t0 (T8103) | 0..1 | 2 | 3 | none |
| v3-t0 (T6000) | 0..3 | 4..7 | 8 | none |
| v3-t1 (T8112) | 0..3 | 4..7 | 8 | none |
| v3-t2 (T6020) | 0..3 | 4..7 | 8 | none |
| v4-t0 (T8122) | 0..3 | 4..7 | 8 | 9..10 |
| v5-t0 (T8140) | 0..1 | 4 | 8 | 9 |
| v5-t1 (T8132) | 0..3 | 4..7 | 8 | 9..11 |
| v5-t2 (T6040) | 0..3 | 4..7 | 8 | 9..11 |

The T6040 row is inferred from the T8132 pool, not measured on silicon; its
inferred mask is 15. Other rows follow the public codec variant tables already
used by `avd/multi-vp`. The non-LSR T6020 image reuses the v3-t1 tunable set,
as in that earlier implementation. This image association still needs device
qualification. No register probing or guessed engine counts are needed in the
host driver.

SRAM programs remain globally ordered and a VP owns its picture state until
that engine's DONE. Different VPs may execute simultaneously, but PP always
starts in descriptor submission order, including when later VPs finish first.
Each frame keeps a distinct instruction FIFO and its buffers until the full
PP completion. Tagged VP notifications and their outbox backpressure work
independently for each engine; they do not release a FIFO or descriptor.
H.264/HEVC VP work reads the picture's bitstream and private slice/scratch state;
reference reconstruction is consumed by ordered PP. VP9/AV1 host dependency
checks remain necessary: selecting another VP cannot satisfy a CPU probability,
CDF or segmentation input dependency.

`make test` checks every pool pair plus the scheduler ownership cases.
`sh tests/build-all.sh` builds all eight images. Run
`python3 tests/transport-test.py build/avd-fw-*.bin` with Unicorn to exercise
all image pools, reverse VP completion, per-engine tagged VP events, ordered PP,
outbox stalls, codec/engine rejection, SRAM streaming and legacy PIODMA.
These tests model completion events and do not decode pixels or measure speed.
