# Open firmware frame queue, ABI 1

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
| 0x04 | ABI version, 1 |
| 0x08 | descriptor count, 16 |
| 0x0c | descriptor stride, 64 |
| 0x10 | SRAM aperture address in the four-byte doorbell address space |
| 0x14 | descriptor ring offset, `0x8000` |
| 0x18 | maximum payload words per program packet, 256 |
| 0x1c | PIODMA destination header for every packet (use verbatim) |
| 0x20 | VP register index limit (exclusive), 4 / 9 / 12 |
| 0x24 | codec mask: bits HEVC=0, H.264=1, VP9=2, AV1=3 |
| 0x28..0x3c | reserved, zero |

Read the block after boot-ready. Missing magic means old firmware: use the
legacy backend, or reject queue mode. An unknown version must not be submitted.
Check count/stride/packet size and the actual SRAM resource length. Use the
advertised address instead of deriving it from the host physical address.

The ring occupies `+0x8000..+0x83ff`. Firmware private state is at
`+0x6000..+0x7fbf`, and its 1024-byte PIODMA staging buffer is at `+0x9000`.
The minimum SRAM size supported by the existing images is `0xc000`. The stack
stays at the existing per-image SRAM end. Do not use these regions for codec
scratch or legacy commands in queue mode.

Current firmware aperture addresses are:

| Image | SRAM address | PIODMA CM3 base | VP FIFO / PP FIFO offset from decode base |
| --- | --- | --- | --- |
| v2-t0 | `0x0108c000` | `0x40070000` | `0x4004 + 4*vp` / `0x4014` |
| v3-t0, v3-t1 | `0x01090000` | `0x40070000` | `0x000c + 4*vp` / `0x0030` |
| v4-t0 | `0x01092000` | `0x41070000` | `0x000c + 4*vp` / `0x0040` |
| v5-t0 | `0x01090000` | `0x41070000` | `0x000c + 4*vp` / `0x0040` |
| v5-t1, v5-t2 | `0x01092000` | `0x41070000` | `0x000c + 4*vp` / `0x0040` |

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
| 0x0c | vp_slot | host: hardware VP index from SoC codec match data |
| 0x10 | program_lo | host: low 32 bits of packet-buffer PIODMA IOVA |
| 0x14 | program_hi | host: high 32 bits of that IOVA |
| 0x18 | program_words | host: instruction words, excludes packet headers/padding |
| 0x1c | pp_kick | host: exact single PP FIFO execution command |
| 0x20 | state | host publishes 1 (SUBMITTED); firmware publishes 2 (COMPLETE) |
| 0x24 | status | firmware: 0 success, 1 invalid descriptor, 2 fatal pipeline I/O |
| 0x28 | completion_tag | firmware: full unmodified 32-bit submitted tag |
| 0x2c..0x3c | reserved[5] | host: zero |

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

## Program packets and PIODMA

`program_words` is in `[1, 0x100000]`. The IOVA is nonzero, four-byte aligned,
and must leave space for all packets without wrapping u64. Allocate
`ceil(program_words / 256) * 1028` bytes. Every packet has fixed stride 1028:

```
packet[0]       = info.packet_header;      // same fixed destination each time
packet[1..256]  = up to 256 instruction words; zero-pad final unused words
```

For packet p, firmware fetches from `program_iova + p * 1028`. It writes low/high
IOVA at PIODMA `+0x4c/+0x50` and starts command `((payload_words + 1) << 6) | 0x11`
at `+0x54`. The size includes the four-byte packet header. It clears stale DONE
at `+0x04` before issuing, polls DONE bit 0 without masking interrupts, then
acknowledges DONE. Polling has a bounded software iteration timeout.

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

```sh
make test
sh tests/build-all.sh
python3 tests/check-images.py build/avd-fw-*.bin
meson setup build/meson --cross-file llvm.ini --buildtype=release
meson compile -C build/meson
meson test -C build/meson --print-errorlogs

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
