/* SPDX-License-Identifier: MIT */
#include "queue.h"

void avdq_init(struct avdq *q, const struct avdq_variant *variant)
{
	unsigned int i;
	q->count = q->halted = q->stray_pp = 0;
	q->pp_active = -1;
	q->variant = variant;
	for (i = 0; i < 4; i++)
		q->next_vp[i] = 0;
	for (i = 0; i < AVDQ_COUNT; i++)
		q->frame[i].phase = AVDQ_FREE;
}

static int normalize(const struct avdq *q, const struct avdq_desc *wire,
		     struct avdq_work *d, unsigned int idx)
{
	uint64_t addr = ((uint64_t)wire->program_hi << 32) | wire->program_lo;
	uint64_t packets = ((uint64_t)wire->program_words + AVDQ_PACKET_WORDS - 1u) /
			   AVDQ_PACKET_WORDS;
	unsigned int i;
	d->magic = wire->magic;
	d->tag = wire->tag;
	d->codec = wire->codec;
	d->fifo = (wire->pp_kick >> 4) & 15u;
	d->program_lo = wire->program_lo;
	d->program_hi = wire->program_hi;
	d->program_words = wire->program_words;
	d->state = wire->state;
	d->pp_kick = wire->pp_kick;
	d->pp_cont = wire->pp_cont;
	d->flags = wire->reserved[0];
	d->extended = !!(d->flags & AVDQ_FLAG_MULTI_VP);
	d->context = 0; /* Legacy ABI 1 publishes in overall submission order. */
	d->dependency = AVDQ_DEP_NONE;
	d->fifo_lo = d->fifo_hi = 0;
	d->num_segments = 1;
	if (d->flags & ~(AVDQ_FLAG_MULTI_VP | AVDQ_TRANSPORT_FLAGS))
		return 0;
	if (d->extended) {
		uint64_t fifo = ((uint64_t)wire->reserved[3] << 32) | wire->reserved[2];
		if ((wire->reserved[1] & ~(AVDQ_CONTEXT_MASK | AVDQ_CONTEXT_DEP)) ||
		    !fifo || (fifo & 255u) || (q->variant->lsr && fifo >= (1ull << 38)))
			return 0;
		d->context = wire->reserved[1] & AVDQ_CONTEXT_MASK;
		d->dependency = !!(wire->reserved[1] & AVDQ_CONTEXT_DEP);
		d->fifo_lo = wire->reserved[2];
		d->fifo_hi = wire->reserved[3];
		d->num_segments = wire->reserved[4];
		if (d->fifo != idx)
			return 0;
	} else {
		if (wire->pp_extra > AVDQ_MAX_PP_EXTRA ||
		    (wire->pp_extra && !wire->pp_cont) || wire->tail[0] || wire->tail[1])
			return 0;
		d->num_segments = wire->pp_extra + 1u;
	}
	if (d->magic != AVDQ_MAGIC || (d->tag & 15u) != idx ||
	    d->state != AVDQ_SUBMITTED || d->codec > AVDQ_AV1 ||
	    !q->variant->count[d->codec] ||
	    (!d->extended && (wire->vp_slot < q->variant->base[d->codec] ||
	     wire->vp_slot >= q->variant->base[d->codec] + q->variant->count[d->codec])) ||
	    d->fifo >= q->variant->fifo_slots || !wire->pp_kick ||
	    !d->program_words || d->program_words > AVDQ_MAX_WORDS ||
	    !d->num_segments || d->num_segments > d->program_words)
		return 0;
	if (d->flags & AVDQ_FLAG_SRAM) {
		if (wire->program_hi)
			return 0; /* program_lo is a free-running word position, including zero. */
	} else if (!addr || (addr & 3u) ||
		   addr > UINT64_MAX - packets * AVDQ_PACKET_BYTES) {
		return 0;
	}
	/* A FIFO stays owned through PP and notification publication. */
	for (i = 0; i < q->count; i++) {
		const struct avdq_frame *other = &q->frame[q->order[i]];
		if (other->status == AVDQ_OK && other->work.fifo == d->fifo)
			return 0;
	}
	return 1;
}

int avdq_submit(struct avdq *q, unsigned int idx, const struct avdq_desc *d)
{
	struct avdq_frame *f;
	if (idx >= AVDQ_COUNT || q->count == AVDQ_COUNT ||
	    q->frame[idx].phase != AVDQ_FREE || q->halted)
		return -1;
	f = &q->frame[idx];
	f->desc = *d;
	f->pushed = f->configured = 0;
	f->vp = AVDQ_VPS;
	f->status = normalize(q, d, &f->work, idx) ? AVDQ_OK : AVDQ_INVALID;
	f->phase = f->status ? AVDQ_DONE : AVDQ_PENDING;
	q->order[q->count++] = idx;
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
		struct avdq_frame *f = &q->frame[q->order[i]];
		if (f->phase == AVDQ_VP && f->vp == vp) {
			f->phase = AVDQ_PP_READY;
			return;
		}
	}
	avdq_fault(q);
}

void avdq_pp_done(struct avdq *q)
{
	if (q->halted)
		return;
	if (q->pp_active < 0) {
		q->stray_pp++; /* Never retire a frame without a PP owner. */
		return;
	}
	q->frame[q->pp_active].phase = AVDQ_DONE;
	q->pp_active = -1;
}

static int predecessor(const struct avdq *q, unsigned int pos, int unfinished)
{
	unsigned int i;
	const struct avdq_frame *f = &q->frame[q->order[pos]];
	for (i = 0; i < pos; i++) {
		const struct avdq_frame *p = &q->frame[q->order[i]];
		if (p->work.context == f->work.context &&
		    (!unfinished || p->phase != AVDQ_DONE))
			return 1;
	}
	return 0;
}

static int free_vp(struct avdq *q, unsigned int codec)
{
	unsigned int i, j, count = q->variant->count[codec];
	for (i = 0; i < count; i++) {
		unsigned int off = (q->next_vp[codec] + i) % count;
		unsigned int vp = q->variant->base[codec] + off;
		for (j = 0; j < q->count; j++) {
			struct avdq_frame *f = &q->frame[q->order[j]];
			if ((f->phase == AVDQ_PUSH || f->phase == AVDQ_VP) && f->vp == vp)
				break;
		}
		if (j == q->count) {
			q->next_vp[codec] = (off + 1) % count;
			return (int)vp;
		}
	}
	return -1;
}

void avdq_step(struct avdq *q, const struct avdq_ops *ops, void *cookie)
{
	unsigned int i, j;
	/* Separate contexts do not block each other's ordered completion stream. */
	for (i = 0; i < q->count; i++) {
		unsigned int idx = q->order[i];
		struct avdq_frame *f = &q->frame[idx];
		if (f->phase == AVDQ_DONE && !predecessor(q, i, 0) &&
		    ops->complete(cookie, idx, f)) {
			f->phase = AVDQ_FREE;
			for (j = i + 1; j < q->count; j++)
				q->order[j - 1] = q->order[j];
			q->count--;
			break;
		}
	}
	if (q->halted)
		return;
	for (i = 0; i < q->count && q->pp_active < 0; i++) {
		unsigned int idx = q->order[i];
		struct avdq_frame *f = &q->frame[idx];
		uint32_t word;
		if (f->phase != AVDQ_PP_READY)
			continue;
		q->pp_active = (int)idx;
		f->phase = AVDQ_PP;
		word = AVDQ_EXEC | (f->work.fifo << 4) | q->variant->fifo_mask;
		if (f->work.codec == AVDQ_VP9 && q->variant->exec_start == 8)
			word |= 0xfff000u;
		ops->kick(cookie, f->work.extended ?
			  word | (1u << q->variant->exec_start) : f->work.pp_kick);
		for (j = 1; j < f->work.num_segments; j++)
			ops->kick(cookie, f->work.extended ? word : f->work.pp_cont);
	}
	/* A single PIODMA staging buffer must stay with its partially fed frame. */
	for (i = 0; i < q->count; i++)
		if (q->frame[q->order[i]].phase == AVDQ_PUSH)
			break;
	if (i == q->count) {
		for (i = 0; i < q->count; i++) {
			struct avdq_frame *f = &q->frame[q->order[i]];
			int vp;
			if (f->phase != AVDQ_PENDING ||
			    (f->work.dependency == AVDQ_DEP_PREV_DONE && predecessor(q, i, 1)))
				continue;
			if (ops->ready) {
				int ready = ops->ready(cookie, f);
				if (ready < 0) {
					avdq_fault(q);
					return;
				}
				if (!ready)
					continue;
			}
			vp = free_vp(q, f->work.codec);
			if (vp < 0)
				continue;
			f->vp = (unsigned int)vp;
			f->phase = AVDQ_PUSH;
			break;
		}
	}
	if (i < q->count) {
		struct avdq_frame *f = &q->frame[q->order[i]];
		int result = ops->feed(cookie, f);
		if (result < 0)
			avdq_fault(q);
		else if (result > 0)
			f->phase = AVDQ_VP;
	}
}
