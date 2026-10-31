/* SPDX-License-Identifier: MIT */
#ifndef AVD_FW_QUEUE_H
#define AVD_FW_QUEUE_H

#include <stdint.h>

#define AVDQ_VERSION 1u
#define AVDQ_MAGIC 0x41565131u /* AVQ1 */
#define AVDQ_COUNT 16u
#define AVDQ_DESC_BYTES 64u
#define AVDQ_RING_OFFSET 0x8000u
#define AVDQ_STAGE_OFFSET 0x9000u
#define AVDQ_PACKET_WORDS 256u
#define AVDQ_PACKET_BYTES ((AVDQ_PACKET_WORDS + 1u) * 4u)
#define AVDQ_MAX_WORDS 0x100000u
#define AVDQ_NOTIFY 0x54530000u
#define AVDQ_FATAL 0x5452ffffu

#define AVDQ_SUBMITTED 1u
#define AVDQ_COMPLETE 2u
#define AVDQ_OK 0u
#define AVDQ_INVALID 1u
#define AVDQ_IO 2u

enum avdq_codec { AVDQ_HEVC, AVDQ_H264, AVDQ_VP9, AVDQ_AV1 };

/* Little-endian wire layout. No native pointers or compiler bitfields. */
struct avdq_desc {
	uint32_t magic, tag, codec, vp_slot;
	uint32_t program_lo, program_hi, program_words, pp_kick;
	uint32_t state, status, completion_tag, reserved[5];
};
_Static_assert(sizeof(struct avdq_desc) == AVDQ_DESC_BYTES, "wire size");

/* The packet header encodes a destination in the 128-KiB SRAM aperture. */
static inline uint32_t avdq_packet_header(uint32_t dst)
{
	return (dst & 0xfffeu) | ((dst & 0x10000u) << 1) | 1u;
}

static inline uint32_t avdq_dma_command(uint32_t words)
{
	return ((words + 1u) << 6) | 0x11u;
}

static inline uint32_t avdq_notification(uint32_t tag, unsigned int idx)
{
	return AVDQ_NOTIFY | (tag & 0xfff0u) | idx;
}

/* Return -1 before doing any aperture read for an unaligned/outside token. */
static inline int avdq_index(uint32_t token, uint32_t ring)
{
	uint32_t offset = token - ring;
	if (token < ring || offset >= AVDQ_COUNT * AVDQ_DESC_BYTES ||
	    offset % AVDQ_DESC_BYTES)
		return -1;
	return (int)(offset / AVDQ_DESC_BYTES);
}

enum avdq_phase { AVDQ_FREE, AVDQ_PENDING, AVDQ_PUSH, AVDQ_VP,
	AVDQ_PP_READY, AVDQ_PP, AVDQ_DONE };
struct avdq_frame {
	struct avdq_desc desc;
	uint32_t phase, pushed, status;
};
struct avdq {
	struct avdq_frame frame[AVDQ_COUNT];
	uint32_t order[AVDQ_COUNT], head, count, halted;
	int pp_active;
};

/* Single owner: IRQs only latch events, the foreground calls these methods. */
struct avdq_ops {
	/* Feed a bounded amount: 0 waiting, 1 all words sent, -1 fatal I/O. */
	int (*feed)(void *cookie, struct avdq_frame *f);
	void (*kick)(void *cookie, uint32_t word);
	/* Must publish record before notification; return 0 if outbox full. */
	int (*complete)(void *cookie, unsigned int idx,
			const struct avdq_frame *f);
};
void avdq_init(struct avdq *q);
int avdq_submit(struct avdq *q, unsigned int idx, const struct avdq_desc *d,
	       unsigned int vp_count, unsigned int codec_mask);
void avdq_fault(struct avdq *q);
void avdq_vp_done(struct avdq *q, unsigned int vp);
void avdq_pp_done(struct avdq *q);
void avdq_step(struct avdq *q, const struct avdq_ops *ops, void *cookie);
#endif
