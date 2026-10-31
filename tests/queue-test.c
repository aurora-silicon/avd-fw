/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "queue.h"

struct fake {
	unsigned int feeds, kicks, completions, blocked, fail;
	uint32_t tags[128], statuses[128], words[128];
};
static int feed(void *cookie, struct avdq_frame *f)
{
	struct fake *io = cookie;
	io->feeds++;
	if (io->fail)
		return -1;
	if (io->blocked)
		return 0;
	return ++f->pushed == f->desc.program_words;
}
static void kick(void *cookie, uint32_t word)
{
	struct fake *io = cookie;
	io->words[io->kicks++] = word;
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
static struct avdq_desc descriptor(unsigned int tag, unsigned int context, unsigned int codec)
{
	struct avdq_desc d = {0};
	d.magic = AVDQ_MAGIC;
	d.tag = tag;
	d.codec = codec;
	d.pp_kick = AVDQ_EXEC | ((tag & 15u) << 4);
	d.reserved[0] = AVDQ_FLAG_MULTI_VP;
	d.program_lo = 0x4000;
	d.program_hi = 1;
	d.program_words = d.reserved[4] = 1;
	d.reserved[1] = context;
	d.state = AVDQ_SUBMITTED;
	d.reserved[2] = 0x1000000 + (tag & 15u) * 0x1000000;
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
	assert(offsetof(struct avdq_desc, reserved) == 0x2c);
	for (i = 0; i < 16; i++) {
		assert(avdq_index(0x1098000u + i * 64, 0x1098000u) == (int)i);
		assert(avdq_index(0x1098001u + i * 64, 0x1098000u) < 0);
	}
	assert(avdq_index(0x1097fffu, 0x1098000u) < 0);
	assert(avdq_index(0x1098400u, 0x1098000u) < 0);
	assert(avdq_notification(0xffffffffu, 15) == 0x5453ffffu);
	assert(avdq_notification(0xffffffffu, 15) != AVDQ_FATAL);
	assert(avdq_dma_command(256) == 0x10111);
	for (i = 0; i < 0x20000; i += 4) {
		uint32_t x = avdq_packet_header(i) & ~0x3fd0001u;
		assert(((x & 0x1ffffu) | ((x & 0x20000u) >> 1)) == i);
	}
}
static void invalid(void)
{
	unsigned int n;
	for (n = 0; n < 18; n++) {
		struct avdq q;
		struct fake io = {0};
		struct avdq_desc d = descriptor(0x100, 1, AVDQ_HEVC);
		avdq_init(&q, &avdq_variants[1]);
		switch (n) {
		case 0: d.magic = 0; break;
		case 1: d.tag++; break;
		case 2: d.state = AVDQ_COMPLETE; break;
		case 3: d.codec = 32; break;
		case 4: d.codec = AVDQ_AV1; break;
		case 5: d.pp_kick ^= 0x10; break;
		case 6: d.program_lo = d.program_hi = 0; break;
		case 7: d.program_lo++; break;
		case 8: d.program_words = 0; break;
		case 9: d.program_words = AVDQ_MAX_WORDS + 1; break;
		case 10: d.program_lo = 0xfffffffcu; d.program_hi = 0xffffffffu; break;
		case 11: d.reserved[4] = 0; break;
		case 12: d.reserved[0] |= 1; break;
		case 13: d.reserved[4] = 2; break;
		case 14: d.reserved[1] |= 1u << 10; break;
		case 15: d.reserved[2] = d.reserved[3] = 0; break;
		case 16: d.reserved[2]++; break;
		case 17: d.reserved[2] = 0; d.reserved[3] = 64; break;
		}
		assert(!avdq_submit(&q, 0, &d));
		assert(q.frame[0].phase == AVDQ_DONE);
		io.blocked = 1; step(&q, &io);
		assert(q.count == 1 && !io.completions && !io.feeds);
		io.blocked = 0; step(&q, &io);
		assert(!q.count && io.statuses[0] == AVDQ_INVALID);
	}
}
static void pools(const struct avdq_variant *v)
{
	unsigned int codec, i;
	for (codec = 0; codec < 4; codec++) {
		struct avdq q;
		struct fake io = {0};
		unsigned int count = v->count[codec];
		avdq_init(&q, v);
		for (i = 0; i < count + 1; i++) {
			struct avdq_desc d = descriptor(0x1000 + i, i, codec);
			assert(!avdq_submit(&q, i, &d));
			step(&q, &io);
		}
		if (!count) {
			assert(io.statuses[0] == AVDQ_INVALID && !io.feeds);
			continue;
		}
		assert(q.frame[count].phase == AVDQ_PENDING);
		for (i = 0; i < count; i++) {
			assert(q.frame[i].phase == AVDQ_VP);
			assert(q.frame[i].vp == v->base[codec] + i);
		}
		/* Reverse VP completion, one PP at a time, no global head blocking. */
		avdq_vp_done(&q, v->base[codec] + count - 1); step(&q, &io);
		assert(q.frame[count].phase == AVDQ_VP);
		assert(q.frame[count].vp == v->base[codec] + count - 1);
		avdq_pp_done(&q); step(&q, &io);
		assert(io.tags[0] == 0x1000 + count - 1);
		assert(io.words[0] == (AVDQ_EXEC | ((count - 1) << 4) | v->fifo_mask |
			(1u << v->exec_start) |
			(codec == AVDQ_VP9 && v->exec_start == 8 ? 0xfff000u : 0)));
	}
}
static void dependencies(void)
{
	struct avdq q;
	struct fake io = {0};
	struct avdq_desc a = descriptor(0x1000, 42, AVDQ_HEVC);
	struct avdq_desc b = descriptor(0x2001, 42, AVDQ_HEVC);
	struct avdq_desc c = descriptor(0x3002, 99, AVDQ_HEVC);
	b.reserved[1] |= AVDQ_CONTEXT_DEP;
	a.program_words = a.reserved[4] = 3;
	avdq_init(&q, &avdq_variants[1]);
	avdq_submit(&q, 0, &a); avdq_submit(&q, 1, &b); avdq_submit(&q, 2, &c);
	for (unsigned int i = 0; i < 5; i++) step(&q, &io);
	assert(q.frame[0].vp == 0 && q.frame[0].phase == AVDQ_VP);
	assert(q.frame[1].phase == AVDQ_PENDING);
	assert(q.frame[2].vp == 1 && q.frame[2].phase == AVDQ_VP);
	avdq_vp_done(&q, 0); step(&q, &io);
	assert(io.kicks == 3 && io.words[0] == 0x2b00020f);
	assert(io.words[1] == 0x2b00000f && io.words[2] == 0x2b00000f);
	assert(q.frame[1].phase == AVDQ_PENDING); /* VP DONE is insufficient */
	avdq_pp_done(&q); io.blocked = 1; step(&q, &io);
	assert(q.frame[1].phase == AVDQ_PUSH); /* dependency DONE, outbox blocked */
	io.blocked = 0; step(&q, &io);
	assert(io.tags[0] == a.tag && q.frame[1].phase == AVDQ_VP);
	/* Independent frames of one ctx may finish in reverse, retire in order. */
	avdq_init(&q, &avdq_variants[1]); memset(&io, 0, sizeof(io));
	a.program_words = a.reserved[4] = 1; b.reserved[1] &= ~AVDQ_CONTEXT_DEP;
	avdq_submit(&q, 0, &a); avdq_submit(&q, 1, &b);
	step(&q, &io); step(&q, &io);
	avdq_vp_done(&q, 1); step(&q, &io); avdq_pp_done(&q); step(&q, &io);
	assert(!io.completions);
	avdq_vp_done(&q, 0); step(&q, &io); avdq_pp_done(&q); step(&q, &io); step(&q, &io);
	assert(io.tags[0] == a.tag && io.tags[1] == b.tag);
}
static void caps_errors(void)
{
	struct avdq q;
	struct fake io = {0};
	struct avdq_desc d;
	unsigned int i;
	avdq_init(&q, &avdq_variants[0]);
	for (i = 0; i < 16; i++) {
		d = descriptor(i, i, AVDQ_HEVC);
		assert(!avdq_submit(&q, i, &d));
		assert(q.frame[i].status == (i < 7 ? AVDQ_OK : AVDQ_INVALID));
	}
	assert(avdq_submit(&q, 0, &d) < 0);
	io.fail = 1; step(&q, &io);
	assert(q.halted);
	for (i = 0; i < 16; i++) step(&q, &io);
	assert(!q.count);
	avdq_vp_done(&q, 0); avdq_pp_done(&q);
	assert(!io.kicks);
	avdq_init(&q, &avdq_variants[1]); avdq_pp_done(&q); assert(q.halted);
	avdq_init(&q, &avdq_variants[1]); avdq_vp_done(&q, 0); assert(q.halted);
}
static void legacy_abi_one(void)
{
	struct avdq q;
	struct fake io = {0};
	struct avdq_desc d = descriptor(0x1000, 0, AVDQ_HEVC);

	memset(d.reserved, 0, sizeof(d.reserved));
	d.pp_kick = 0x2b000400;
	avdq_init(&q, &avdq_variants[1]);
	assert(!avdq_submit(&q, 0, &d));
	step(&q, &io);
	assert(q.frame[0].phase == AVDQ_VP && !q.frame[0].work.extended);
	avdq_vp_done(&q, 0); step(&q, &io);
	assert(io.words[0] == d.pp_kick);
	avdq_pp_done(&q); step(&q, &io);
	assert(io.completions == 1 && io.tags[0] == d.tag);
	avdq_init(&q, &avdq_variants[1]);
	assert(!avdq_submit(&q, 0, &d));
	d.tag++;
	assert(!avdq_submit(&q, 1, &d));
	assert(q.frame[1].status == AVDQ_INVALID); /* Duplicate FIFO ownership. */
	avdq_init(&q, &avdq_variants[1]);
	memset(&io, 0, sizeof(io));
	d.tag = 0xffffffffu; /* Legacy ring and hardware FIFO indices may differ. */
	assert(!avdq_submit(&q, 15, &d));
	step(&q, &io);
	avdq_vp_done(&q, 0); step(&q, &io);
	avdq_pp_done(&q); step(&q, &io);
	assert(io.tags[0] == 0xffffffffu && !q.count);

}
int main(void)
{
	wire(); invalid(); dependencies(); caps_errors(); legacy_abi_one();
	for (unsigned int i = 0; i < sizeof(avdq_variants) / sizeof(avdq_variants[0]); i++)
		pools(&avdq_variants[i]);
	puts("queue: eight image pools, concurrent contexts, FIFO caps, dependencies, PP continuations, ordered tags, faults passed");
	return 0;
}
