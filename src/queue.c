/* SPDX-License-Identifier: MIT */
#include "queue.h"

void avdq_init(struct avdq *q)
{
	unsigned int i;
	q->head = q->count = q->halted = q->stray_pp = 0;
	q->pp_active = -1;
	for (i = 0; i < AVDQ_COUNT; i++)
		q->frame[i].phase = AVDQ_FREE;
}

static int valid(const struct avdq_desc *d, unsigned int idx,
		 unsigned int vp_count, unsigned int codec_mask)
{
	uint64_t addr = ((uint64_t)d->program_hi << 32) | d->program_lo;
	uint64_t packets = (d->program_words + AVDQ_PACKET_WORDS - 1u) /
			   AVDQ_PACKET_WORDS;
	unsigned int i;
	if (d->magic != AVDQ_MAGIC || (d->tag & 15u) != idx ||
	    d->state != AVDQ_SUBMITTED || d->codec > AVDQ_AV1 ||
	    !(codec_mask & (1u << d->codec)) || d->vp_slot >= vp_count ||
	    !d->program_words || d->program_words > AVDQ_MAX_WORDS ||
	    !d->pp_kick || (d->flags & ~AVDQ_FLAG_SRAM) ||
	    d->pp_extra > AVDQ_MAX_PP_EXTRA || (d->pp_extra && !d->pp_cont))
		return 0;
	if (d->flags & AVDQ_FLAG_SRAM) {
		/* program_lo is the ring write count at the first word. */
		if (d->program_hi)
			return 0;
	} else if (!addr || (addr & 3u) ||
		   addr > UINT64_MAX - packets * AVDQ_PACKET_BYTES) {
		return 0;
	}
	for (i = 0; i < 2; i++)
		if (d->reserved[i])
			return 0;
	return 1;
}

int avdq_submit(struct avdq *q, unsigned int idx, const struct avdq_desc *d,
	       unsigned int vp_count, unsigned int codec_mask)
{
	struct avdq_frame *f;
	/* Duplicate doorbells must never overwrite the owner or its full tag. */
	if (idx >= AVDQ_COUNT || q->count == AVDQ_COUNT ||
	    q->frame[idx].phase != AVDQ_FREE || q->halted)
		return -1;
	f = &q->frame[idx];
	f->desc = *d;
	f->pushed = 0;
	f->status = valid(d, idx, vp_count, codec_mask) ? AVDQ_OK : AVDQ_INVALID;
	f->phase = f->status ? AVDQ_DONE : AVDQ_PENDING;
	q->order[(q->head + q->count) & (AVDQ_COUNT - 1)] = idx;
	q->count++;
	return 0;
}

void avdq_fault(struct avdq *q)
{
	unsigned int i;
	q->halted = 1;
	q->pp_active = -1;
	for (i = 0; i < AVDQ_COUNT; i++) {
		struct avdq_frame *f = &q->frame[i];
		if (f->phase != AVDQ_FREE && f->phase != AVDQ_DONE) {
			f->status = AVDQ_IO;
			f->phase = AVDQ_DONE;
		}
	}
}

void avdq_vp_done(struct avdq *q, unsigned int vp)
{
	unsigned int i;
	if (q->halted)
		return;
	for (i = 0; i < q->count; i++) {
		struct avdq_frame *f = &q->frame[q->order[(q->head + i) & 15u]];
		if (f->phase == AVDQ_VP && f->desc.vp_slot == vp) {
			f->phase = AVDQ_PP_READY;
			return;
		}
	}
	/* Lost/coalesced or premature DONE cannot be attributed safely. */
	avdq_fault(q);
}

void avdq_pp_done(struct avdq *q)
{
	if (q->halted)
		return;
	if (q->pp_active < 0) {
		/*
		 * Multi-kick frames may raise more than one PP DONE. The legacy
		 * host completes on the first and ignores the rest; do the same
		 * rather than poisoning the queue.
		 */
		q->stray_pp++;
		return;
	}
	q->frame[q->pp_active].phase = AVDQ_DONE;
	q->pp_active = -1;
}

void avdq_step(struct avdq *q, const struct avdq_ops *ops, void *cookie)
{
	unsigned int i;
	/* Publish in submission order; a full outbox never drops a completion. */
	if (q->count) {
		unsigned int idx = q->order[q->head];
		struct avdq_frame *f = &q->frame[idx];
		if (f->phase == AVDQ_DONE && ops->complete(cookie, idx, f)) {
			f->phase = AVDQ_FREE;
			q->head = (q->head + 1u) & 15u;
			q->count--;
		}
	}
	if (q->halted)
		return;
	for (i = 0; i < q->count; i++) {
		unsigned int idx = q->order[(q->head + i) & 15u];
		struct avdq_frame *f = &q->frame[idx];
		if (f->phase == AVDQ_PP_READY && q->pp_active < 0) {
			q->pp_active = (int)idx;
			f->phase = AVDQ_PP;
			ops->kick(cookie, &f->desc);
			break;
		}
	}
	/* One SRAM staging buffer; preserve program order across chunks. */
	for (i = 0; i < q->count; i++) {
		struct avdq_frame *f = &q->frame[q->order[(q->head + i) & 15u]];
		int result;
		if (f->phase != AVDQ_PENDING && f->phase != AVDQ_PUSH)
			continue;
		f->phase = AVDQ_PUSH;
		result = ops->feed(cookie, f);
		if (result < 0)
			avdq_fault(q);
		else if (result > 0)
			f->phase = AVDQ_VP;
		break;
	}
}
