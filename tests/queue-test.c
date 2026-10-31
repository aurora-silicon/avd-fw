/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "queue.h"

struct fake {
	unsigned int feeds, kicks, completions, blocked, fail;
	uint32_t tags[64], statuses[64], words[64];
};
static int feed(void *cookie, struct avdq_frame *f)
{
	struct fake *io = cookie;
	io->feeds++;
	if (io->fail)
		return -1;
	if (io->blocked)
		return 0;
	f->pushed++;
	return f->pushed == f->desc.program_words;
}
static void kick(void *cookie, const struct avdq_desc *d)
{
	struct fake *io = cookie;
	io->words[io->kicks++] = d->pp_kick;
}
static int complete(void *cookie, unsigned int idx, const struct avdq_frame *f)
{
	struct fake *io = cookie;
	assert(idx < AVDQ_COUNT);
	if (io->blocked)
		return 0;
	io->tags[io->completions] = f->desc.tag;
	io->statuses[io->completions++] = f->status;
	return 1;
}
static const struct avdq_ops ops = { feed, kick, complete };
static struct avdq_desc descriptor(unsigned int tag, unsigned int vp)
{
	struct avdq_desc d = {0};
	d.magic = AVDQ_MAGIC;
	d.tag = tag;
	d.codec = AVDQ_HEVC;
	d.vp_slot = vp;
	d.program_lo = 0x4000;
	d.program_hi = 1; /* exercise a program above 4 GiB */
	d.program_words = 1;
	d.pp_kick = 0x2b000400u | (tag & 15u) << 4;
	d.state = AVDQ_SUBMITTED;
	return d;
}
static void step(struct avdq *q, struct fake *io)
{
	avdq_step(q, &ops, io);
}
static void wire(void)
{
	unsigned int i;
	assert(offsetof(struct avdq_desc, program_lo) == 0x10);
	assert(offsetof(struct avdq_desc, state) == 0x20);
	assert(offsetof(struct avdq_desc, completion_tag) == 0x28);
	for (i = 0; i < 16; i++) {
		assert(avdq_index(0x1098000u + i * 64, 0x1098000u) == (int)i);
		assert(avdq_index(0x1098001u + i * 64, 0x1098000u) < 0);
	}
	assert(avdq_index(0x1097fffu, 0x1098000u) < 0);
	assert(avdq_index(0x1098400u, 0x1098000u) < 0);
	assert(avdq_index(0xffffffffu, 0x1098000u) < 0);
	assert(avdq_notification(0x12345678u, 8) == 0x54535678u);
	assert(avdq_notification(0xffffffffu, 15) == 0x5453ffffu);
	assert(avdq_notification(0xffffffffu, 15) != AVDQ_FATAL);
	assert(avdq_dma_command(1) == 0x211);
	assert(avdq_dma_command(256) == 0x10111);
	/* Cross-check byte units against the existing Tansy transport packets. */
	assert(avdq_dma_command(221) == 0xde11); /* HEADER */
	assert(avdq_dma_command(11) == 0x0c11); /* WORK */
	assert(avdq_dma_command(91) == 0x5c11); /* SLICE: 91-word continuation */
	for (i = 0; i < 0x20000; i += 4) {
		uint32_t h = avdq_packet_header(i);
		/* Destination decode from the MIT public emulator. */
		uint32_t x = h & ~0x3fd0001u;
		assert(((x & 0x1ffffu) | ((x & 0x20000u) >> 1)) == i);
	}
}
static void invalid(void)
{
	unsigned int n;
	for (n = 0; n < 18; n++) {
		struct avdq q;
		struct fake io = {0};
		struct avdq_desc d = descriptor(0x100, 0);
		avdq_init(&q);
		switch (n) {
		case 0: d.magic = 0; break;
		case 1: d.tag++; break;
		case 2: d.state = AVDQ_COMPLETE; break;
		case 3: d.codec = 32; break;
		case 4: d.codec = AVDQ_AV1; break;
		case 5: d.vp_slot = 9; break;
		case 6: d.program_lo = d.program_hi = 0; break;
		case 7: d.program_lo++; break;
		case 8: d.program_words = 0; break;
		case 9: d.program_words = AVDQ_MAX_WORDS + 1; break;
		case 10: d.program_lo = 0xfffffffcu; d.program_hi = 0xffffffffu; break;
		case 11: d.pp_kick = 0; break;
		case 12: d.reserved[1] = 1; break;
		case 13: d.flags = 2; break;
		case 14: d.pp_extra = AVDQ_MAX_PP_EXTRA + 1; d.pp_cont = 1; break;
		case 15: d.pp_extra = 1; break; /* continuation word missing */
		case 16: d.flags = AVDQ_FLAG_SRAM; break; /* program_hi must be 0 */
		case 17: d.flags = AVDQ_FLAG_SRAM; d.program_hi = 0; d.program_words = 0; break;
		}
		assert(!avdq_submit(&q, 0, &d, 9, 7));
		assert(q.frame[0].phase == AVDQ_DONE);
		/* Tag mismatch also gets echoed without masking the full generation. */
		io.blocked = 1;
		step(&q, &io);
		assert(q.count == 1 && io.completions == 0 && io.feeds == 0);
		io.blocked = 0; step(&q, &io);
		assert(!q.count && io.completions == 1);
		assert(io.tags[0] == d.tag && io.statuses[0] == AVDQ_INVALID);
		avdq_init(&q);
	}
}
static void pipeline(void)
{
	struct avdq q;
	struct fake io = {0};
	struct avdq_desc a = descriptor(0x12340000, 0);
	struct avdq_desc b = descriptor(0x56780001, 0);
	struct avdq_desc c = descriptor(0xabcd0002, 1);
	avdq_init(&q);
	assert(!avdq_submit(&q, 0, &a, 12, 15));
	assert(!avdq_submit(&q, 1, &b, 12, 15));
	assert(!avdq_submit(&q, 2, &c, 12, 15));
	assert(avdq_submit(&q, 0, &b, 12, 15) == -1);
	step(&q, &io); step(&q, &io); step(&q, &io);
	assert(q.frame[0].phase == AVDQ_VP && q.frame[1].phase == AVDQ_VP);
	assert(io.feeds == 3 && !io.kicks && !io.completions);
	/* A later VP can finish first; PP still runs in submission order. */
	avdq_vp_done(&q, 1); step(&q, &io);
	assert(!io.kicks && q.frame[2].phase == AVDQ_PP_READY);
	avdq_vp_done(&q, 0); avdq_vp_done(&q, 0); step(&q, &io);
	assert(io.kicks == 1 && io.words[0] == a.pp_kick);
	assert(q.frame[1].phase == AVDQ_PP_READY);
	avdq_pp_done(&q); step(&q, &io);
	assert(io.kicks == 2 && io.words[1] == b.pp_kick && io.completions == 1);
	avdq_pp_done(&q);
	io.blocked = 1; step(&q, &io);
	assert(q.count == 2 && io.completions == 1 && io.kicks == 3);
	assert(io.words[2] == c.pp_kick);
	io.blocked = 0; step(&q, &io);
	avdq_pp_done(&q); step(&q, &io); step(&q, &io);
	assert(!q.count && io.completions == 3);
	assert(io.tags[0] == a.tag && io.tags[1] == b.tag && io.tags[2] == c.tag);
	assert(!io.statuses[0] && !io.statuses[1] && !io.statuses[2]);
}
static void wrap_and_full(void)
{
	struct avdq q;
	struct fake io = {0};
	unsigned int round, i;
	avdq_init(&q);
	for (round = 0; round < 3; round++) {
		for (i = 0; i < 16; i++) {
			struct avdq_desc d = descriptor((round << 28) | i, 0);
			assert(!avdq_submit(&q, i, &d, 12, 15));
		}
		assert(q.count == 16);
		assert(avdq_submit(&q, 16, &q.frame[0].desc, 12, 15) == -1);
		for (i = 0; i < 16; i++) step(&q, &io);
		for (i = 0; i < 16; i++) {
			avdq_vp_done(&q, 0); step(&q, &io);
			avdq_pp_done(&q); step(&q, &io);
		}
		assert(q.count == 0 && q.head == 0 && !q.halted);
	}
	assert(io.completions == 48);
	assert(io.tags[32] == 0x20000000);
}
static void partial_program(void)
{
	struct avdq q;
	struct fake io = {0};
	struct avdq_desc a = descriptor(0, 0), b = descriptor(1, 1);
	unsigned int i;
	a.program_words = 513;
	avdq_init(&q);
	avdq_submit(&q, 0, &a, 12, 15); avdq_submit(&q, 1, &b, 12, 15);
	io.blocked = 1; step(&q, &io);
	assert(q.frame[0].pushed == 0 && q.frame[1].phase == AVDQ_PENDING);
	io.blocked = 0;
	for (i = 0; i < 512; i++) step(&q, &io);
	assert(q.frame[0].phase == AVDQ_PUSH && !q.frame[1].pushed);
	step(&q, &io);
	assert(q.frame[0].phase == AVDQ_VP && !q.frame[1].pushed);
	step(&q, &io);
	assert(q.frame[1].phase == AVDQ_VP && q.frame[0].pushed == 513);
}
static void arbitrary_indices(void)
{
	struct avdq q;
	struct fake io = {0};
	struct avdq_desc a = descriptor(0x1007, 0), b = descriptor(0x2003, 0);
	avdq_init(&q);
	assert(!avdq_submit(&q, 7, &a, 12, 15));
	assert(!avdq_submit(&q, 3, &b, 12, 15));
	step(&q, &io); step(&q, &io);
	avdq_vp_done(&q, 0); step(&q, &io); avdq_pp_done(&q); step(&q, &io);
	avdq_vp_done(&q, 0); step(&q, &io); avdq_pp_done(&q); step(&q, &io);
	assert(io.completions == 2 && io.tags[0] == a.tag && io.tags[1] == b.tag);
}
static void errors(void)
{
	struct avdq q;
	struct fake io = {0};
	struct avdq_desc a = descriptor(0, 0), b = descriptor(1, 1);
	avdq_init(&q);
	avdq_submit(&q, 0, &a, 12, 15); avdq_submit(&q, 1, &b, 12, 15);
	io.fail = 1; step(&q, &io);
	assert(q.halted && q.count == 2);
	step(&q, &io); step(&q, &io);
	assert(!q.count && io.statuses[0] == AVDQ_IO && io.statuses[1] == AVDQ_IO);
	assert(avdq_submit(&q, 0, &a, 12, 15) == -1);
	avdq_vp_done(&q, 0); avdq_pp_done(&q); /* late events after a fault */
	assert(!io.kicks);
	/* A stray PP DONE (multi-kick frames) is counted, not fatal. */
	avdq_init(&q);
	avdq_pp_done(&q); assert(!q.halted && q.stray_pp == 1);
	avdq_init(&q);
	avdq_vp_done(&q, 0); assert(q.halted);
}
static void sram_ring(void)
{
	struct avdq q;
	struct fake io = {0};
	struct avdq_desc d = descriptor(0x40000, 0);
	d.flags = AVDQ_FLAG_SRAM;
	d.program_lo = 0xfffffff0u; /* free-running count may wrap */
	d.program_hi = 0;
	d.pp_extra = 3;
	d.pp_cont = 0x2b000000u;
	avdq_init(&q);
	assert(!avdq_submit(&q, 0, &d, 12, 15));
	assert(q.frame[0].phase == AVDQ_PENDING);
	step(&q, &io);
	assert(q.frame[0].phase == AVDQ_VP);
	avdq_vp_done(&q, 0); step(&q, &io);
	assert(io.kicks == 1 && io.words[0] == d.pp_kick);
	avdq_pp_done(&q); step(&q, &io);
	assert(io.completions == 1 && io.statuses[0] == AVDQ_OK);
	avdq_pp_done(&q); avdq_pp_done(&q); step(&q, &io);
	assert(!q.halted && q.stray_pp == 2);
}
int main(void)
{
	wire(); sram_ring(); invalid(); pipeline(); wrap_and_full(); partial_program(); arbitrary_indices(); errors();
	puts("queue: wire, sram ring, validation, pipeline, backpressure, wrap, tags, errors passed");
	return 0;
}
