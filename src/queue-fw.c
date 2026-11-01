/* SPDX-License-Identifier: MIT */
#include "queue.h"
#include "queue-fw.h"
#include "hw/reg/reg.h"

/* Register generations follow the firmware filenames, not host revision IDs. */
#if AVD_VER == 2
#define SRAM_APERTURE 0x0108c000u
#define VP_COUNT 4u
#define VP_FIFO 0x4004u
#define PP_FIFO 0x4014u
#define FIFO_MAX 0x4018u
#define FIFO_CUR 0x402cu
#elif AVD_VER == 3
#define SRAM_APERTURE 0x01090000u
#define VP_COUNT 9u
#define VP_FIFO 0xcu
#define PP_FIFO 0x30u
#define FIFO_MAX 0x34u
#define FIFO_CUR 0x5cu
#else
#if AVD_VER == 5 && AVD_TIER == 0
#define SRAM_APERTURE 0x01090000u
#elif AVD_VER == 5 && AVD_TIER == 2
/* T6040/T6041: 0x14000-byte code window, so SRAM starts at +0x94000. */
#define SRAM_APERTURE 0x01094000u
#else
#define SRAM_APERTURE 0x01092000u
#endif
#define VP_COUNT 12u
#define VP_FIFO 0xcu
#define PP_FIFO 0x40u
#define FIFO_MAX 0x44u
#define FIFO_CUR 0x7cu
#endif

#if AVD_VER <= 3
#define Q_PIODMA_BASE 0x40070000u
#else
#define Q_PIODMA_BASE 0x41070000u
#endif
/*
 * Independent hardware instruction FIFOs (EXEC bits 7:4).  Public variant
 * tables: 7 on T8103 (v2) and T8140 (v5-t0), 15 on every other part.
 */
#if AVD_VER == 2 || (AVD_VER == 5 && AVD_TIER == 0)
#define FIFO_SLOTS 7u
#else
#define FIFO_SLOTS 15u
#endif
#define Q_DMA(off) REG(Q_PIODMA_BASE + (off))
#define SRAM_ALIAS 0x10000000u
#define Q_RING (SRAM_APERTURE + AVDQ_RING_OFFSET)
#define Q_DESC(i) ((volatile struct avdq_desc *)(SRAM_ALIAS + \
		   AVDQ_RING_OFFSET + (i) * AVDQ_DESC_BYTES))
#define Q_STAGE REG(SRAM_ALIAS + AVDQ_STAGE_OFFSET)
#define Q_INFO REG(SRAM_ALIAS + 0x7fc0u)
#define Q_POOL REG(SRAM_ALIAS + AVDQ_POOL_OFFSET)
#define Q_RING_WORD REG(SRAM_ALIAS + AVDQ_SRAM_RING_OFFSET)
#define Q_INFO_RP 14	/* firmware: ring words consumed */
#define Q_INFO_WP 15	/* host: ring words written */
/* Words fed per foreground step before interrupts are serviced again. */
#define Q_FEED_BATCH 64u
_Static_assert(AVDQ_SRAM_RING_OFFSET + AVDQ_SRAM_RING_WORDS * 4u <= 0x6000u,
	       "SRAM ring overlaps queue state");

/* Explicit SRAM placement: no dependency on the legacy uninitialized BSS. */
struct queue_fw {
	struct avdq q;
	volatile unsigned int mode, fatal, in_write, in_read;
	volatile unsigned int inbox[AVDQ_COUNT];
	volatile unsigned int vp_done[12], pp_done, error;
	unsigned int dma_pending, dma_wait, stage_words, stage_used;
	unsigned int ring_rp;	/* SRAM ring words consumed (free-running) */
};
#define FW ((struct queue_fw *)(SRAM_ALIAS + 0x6000u))
_Static_assert(sizeof(struct queue_fw) <= AVDQ_POOL_OFFSET - 0x6000u, "SRAM queue state overlaps ABI");

static void barrier(void)
{
	__asm volatile("dmb sy" ::: "memory");
}

void avdq_fw_init(void)
{
	unsigned int i;
	volatile unsigned int *p = (volatile unsigned int *)FW;
	for (i = 0; i < sizeof(*FW) / sizeof(*p); i++)
		p[i] = 0;
	avdq_init(&FW->q, AVDQ_IMAGE_POOL);
	Q_POOL[0] = 0;
	Q_POOL[1] = AVDQ_POOL_VERSION;
	for (i = 0; i < 4; i++)
		Q_POOL[2 + i] = AVDQ_IMAGE_POOL->codec[i];
	Q_POOL[6] = AVDQ_IMAGE_POOL->inferred;
	Q_POOL[7] = 0;
	for (i = 0; i < 16; i++)
		Q_INFO[i] = 0;
	Q_INFO[1] = AVDQ_VERSION;
	Q_INFO[2] = AVDQ_COUNT;
	Q_INFO[3] = AVDQ_DESC_BYTES;
	Q_INFO[4] = SRAM_APERTURE;
	Q_INFO[5] = AVDQ_RING_OFFSET;
	Q_INFO[6] = AVDQ_PACKET_WORDS;
	Q_INFO[7] = avdq_packet_header(SRAM_APERTURE + AVDQ_STAGE_OFFSET);
	Q_INFO[8] = VP_COUNT;
	Q_INFO[9] = AVD_VER <= 3 ? 7u : 15u;
	Q_INFO[10] = FIFO_SLOTS;
	Q_INFO[11] = AVDQ_FEAT_SRAM_RING | AVDQ_FEAT_MULTI_KICK |
		     AVDQ_FEAT_VP_DONE | AVDQ_FEAT_MULTI_VP;
	Q_INFO[12] = AVDQ_SRAM_RING_OFFSET;
	Q_INFO[13] = AVDQ_SRAM_RING_WORDS;
	barrier();
	Q_POOL[0] = AVDQ_POOL_MAGIC;
	Q_INFO[0] = AVDQ_MAGIC;
}

/* The CM3 interrupt router is separate from the NVIC and decoder masks. */
static void queue_enable_irq(unsigned int irq)
{
	unsigned int bank = (irq - 14u) / 32u;
	unsigned int mask = 1u << ((irq - 14u) % 32u);

	reg_write(CM3_MBOX_IRQEN(bank), reg_read(CM3_MBOX_IRQEN(bank)) | mask);
}

static void queue_enable_irqs(unsigned int vp)
{
	unsigned int event, first;

	/* Enable only a validated descriptor's VP, not holes in the VP limit. */
	first = (AVD_VER == 2 ? 18u : 78u) + vp * 5u;
	for (event = 0; event < 3; event++)
		queue_enable_irq(first + event); /* unknown/error/DONE, not FULL */
	first = AVD_VER == 2 ? 38u : 62u;
	queue_enable_irq(first); /* PP unknown */
	queue_enable_irq(first + 2u); /* PP DONE */
}

int avdq_fw_mailbox(unsigned int token)
{
	int idx = avdq_index(token, Q_RING);
	unsigned int write;
	if (idx < 0)
		return FW->mode != 0; /* Never run Tansy resets during a queue session. */
	if (!FW->mode) {
		/* Queue mode owns these routes; legacy mode before it is unchanged. */
		for (unsigned int i = 0; i < 6; i++)
			reg_write(CM3_MBOX_IRQEN(i), 0);
	}
	FW->mode = 1;
	write = FW->in_write;
	if (write - FW->in_read >= AVDQ_COUNT) {
		FW->fatal = 1;
		return 1;
	}
	FW->inbox[write & 15u] = (unsigned int)idx;
	barrier();
	FW->in_write = write + 1u;
	return 1;
}

/* Called only after the hardware source has been acknowledged. */
int avdq_fw_event(unsigned int vp, unsigned int event)
{
	if (!FW->mode)
		return 0;
	if (event == DECODE_STATUS_FULL)
		return 1;
	if (event == DECODE_STATUS_ERR)
		FW->error = 1;
	else if (vp == IRQ_SUBMIT)
		FW->pp_done++;
	else if (vp < VP_COUNT)
		FW->vp_done[vp]++;
	else
		FW->error = 1;
	return 1;
}

/*
 * ABI 2 SRAM ring: the host has already streamed (or is still streaming) the
 * program into the ring at SRAM +0. Copy up to Q_FEED_BATCH words per step
 * into the VP FIFO, never past the host's write count or the FIFO capacity.
 */
static int feed_sram(struct queue_fw *fw, struct avdq_frame *f)
{
	unsigned int max, cur, avail, n;
	if (!f->pushed && f->desc.program_lo != fw->ring_rp)
		return -1; /* host and firmware disagree about the ring position */
	avail = Q_INFO[Q_INFO_WP] - fw->ring_rp;
	if (avail > AVDQ_SRAM_RING_WORDS)
		return -1;
	if (!avail)
		return 0; /* host is still writing; its watchdog covers a stall */
	if (avail > f->desc.program_words - f->pushed)
		avail = f->desc.program_words - f->pushed;
	if (avail > Q_FEED_BATCH)
		avail = Q_FEED_BATCH;
	barrier();
	for (n = 0; n < avail; n++) {
		max = reg_read(REG(DECODE_CTRL_BASE + FIFO_MAX + f->desc.vp_slot * 4u));
		cur = reg_read(REG(DECODE_CTRL_BASE + FIFO_CUR + f->desc.vp_slot * 4u));
		if (!max || cur >= max)
			break;
		reg_write(REG(DECODE_CTRL_BASE + VP_FIFO + f->desc.vp_slot * 4u),
			  Q_RING_WORD[fw->ring_rp % AVDQ_SRAM_RING_WORDS]);
		__asm volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop" ::: "memory");
		fw->ring_rp++;
		f->pushed++;
	}
	if (n) {
		barrier();
		Q_INFO[Q_INFO_RP] = fw->ring_rp;
	}
	return f->pushed == f->desc.program_words;
}

static int feed(void *cookie, struct avdq_frame *f)
{
	struct queue_fw *fw = cookie;
	unsigned int words, max, cur;
	uint64_t addr;
	if (f->desc.flags & AVDQ_FLAG_SRAM)
		return feed_sram(fw, f);
	if (fw->stage_used == fw->stage_words) {
		if (!fw->dma_pending) {
			words = f->desc.program_words - f->pushed;
			if (words > AVDQ_PACKET_WORDS)
				words = AVDQ_PACKET_WORDS;
			addr = ((uint64_t)f->desc.program_hi << 32) | f->desc.program_lo;
			addr += (uint64_t)(f->pushed / AVDQ_PACKET_WORDS) * AVDQ_PACKET_BYTES;
			reg_write(Q_DMA(0x04), 1); /* discard stale DONE before issuing */
			reg_write(Q_DMA(0x4c), (unsigned int)addr);
			reg_write(Q_DMA(0x50), (unsigned int)(addr >> 32));
			barrier();
			reg_write(Q_DMA(0x54), avdq_dma_command(words));
			fw->dma_pending = 1;
			fw->dma_wait = 1000000;
			return 0;
		}
		if (!(reg_read(Q_DMA(0x04)) & 1u))
			return --fw->dma_wait ? 0 : -1;
		reg_write(Q_DMA(0x04), 1);
		barrier();
		fw->stage_words = f->desc.program_words - f->pushed;
		if (fw->stage_words > AVDQ_PACKET_WORDS)
			fw->stage_words = AVDQ_PACKET_WORDS;
		fw->stage_used = fw->dma_pending = 0;
	}
	/* Same per-VP capacity/occupancy registers as Linux's direct FIFO path. */
	max = reg_read(REG(DECODE_CTRL_BASE + FIFO_MAX + f->desc.vp_slot * 4u));
	cur = reg_read(REG(DECODE_CTRL_BASE + FIFO_CUR + f->desc.vp_slot * 4u));
	if (!max || cur >= max)
		return 0; /* Poll later; interrupts/PP/completions continue progressing. */
	reg_write(REG(DECODE_CTRL_BASE + VP_FIFO + f->desc.vp_slot * 4u),
		  Q_STAGE[fw->stage_used++]);
	/* Conservative local store cadence also used by the Tansy HEVC path. */
	__asm volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop" ::: "memory");
	f->pushed++;
	if (f->pushed == f->desc.program_words) {
		fw->stage_words = fw->stage_used = 0;
		return 1;
	}
	return 0;
}

static void kick(void *cookie, const struct avdq_desc *d)
{
	unsigned int i;
	(void)cookie;
	reg_write(REG(DECODE_CTRL_BASE + PP_FIFO), d->pp_kick);
	/* Same continuation EXEC words the legacy host wrote, one per segment. */
	for (i = 0; i < d->pp_extra; i++)
		reg_write(REG(DECODE_CTRL_BASE + PP_FIFO), d->pp_cont);
}

/* VP records do not release any descriptor or DMA storage. */
static void notify_vp(void)
{
	unsigned int i;

	for (i = 0; i < FW->q.count; i++) {
		unsigned int idx = FW->q.order[(FW->q.head + i) & 15u];
		struct avdq_frame *f = &FW->q.frame[idx];
		volatile struct avdq_desc *d = Q_DESC(idx);

		if (!(f->desc.flags & AVDQ_FLAG_VP_DONE) || f->vp_notified ||
		    f->status || (f->phase != AVDQ_PP_READY &&
		    f->phase != AVDQ_PP && f->phase != AVDQ_DONE))
			continue;
		if (!(reg_read(CM3_MBOX1_STATUS) & CM3_MBOX_STATUS_EMPTY))
			return;
		d->reserved[0] = f->desc.tag;
		barrier();
		d->reserved[1] = 1;
		barrier();
		reg_write(CM3_MBOX1_TX, AVDQ_VP_NOTIFY | (f->desc.tag & 0xfff0u) | idx);
		f->vp_notified = 1;
		return;
	}
}

static int complete(void *cookie, unsigned int idx, const struct avdq_frame *f)
{
	volatile struct avdq_desc *d = Q_DESC(idx);
	(void)cookie;
	if (!(reg_read(CM3_MBOX1_STATUS) & CM3_MBOX_STATUS_EMPTY))
		return 0;
	d->status = f->status;
	d->completion_tag = f->desc.tag;
	barrier();
	d->state = AVDQ_COMPLETE;
	barrier();
	reg_write(CM3_MBOX1_TX, avdq_notification(f->desc.tag, idx));
	return 1;
}

int avdq_fw_poll(void)
{
	static const struct avdq_ops ops = { feed, kick, complete };
	unsigned int i, vp_done[12], pp_done, error;
	if (!FW->mode)
		return 0;
	/* IRQ handlers latch only events; all scheduler mutations are foreground. */
	__asm volatile("cpsid i" ::: "memory");
	for (i = 0; i < 12; i++) {
		vp_done[i] = FW->vp_done[i];
		FW->vp_done[i] = 0;
	}
	pp_done = FW->pp_done;
	FW->pp_done = 0;
	error = FW->error;
	FW->error = 0;
	__asm volatile("cpsie i" ::: "memory");
	if (error || FW->fatal)
		avdq_fault(&FW->q);
	for (i = 0; i < VP_COUNT; i++)
		while (vp_done[i]--)
			avdq_vp_done(&FW->q, i);
	while (pp_done--)
		avdq_pp_done(&FW->q);
	if (!FW->q.halted && FW->in_read != FW->in_write) {
		unsigned int idx = FW->inbox[FW->in_read & 15u];
		struct avdq_desc d;
		uint32_t *dst = (uint32_t *)&d;
		volatile unsigned int *src = (volatile unsigned int *)Q_DESC(idx);
		barrier();
		for (i = 0; i < AVDQ_DESC_BYTES / 4; i++)
			dst[i] = src[i];
		FW->in_read++;
		if (avdq_submit(&FW->q, idx, &d, VP_COUNT, AVD_VER <= 3 ? 7u : 15u)) {
			FW->fatal = 1;
			avdq_fault(&FW->q);
		} else if (FW->q.frame[idx].status == AVDQ_OK) {
			queue_enable_irqs(d.vp_slot);
		}
		/* Queue mode owns PIODMA; the Tansy transport cannot run concurrently. */
		reg_write(Q_DMA(0x00), reg_read(Q_DMA(0x00)) & ~1u);
	}
	notify_vp();
	avdq_step(&FW->q, &ops, FW);
	if (FW->fatal && !FW->q.count &&
	    (reg_read(CM3_MBOX1_STATUS) & CM3_MBOX_STATUS_EMPTY)) {
		reg_write(CM3_MBOX1_TX, AVDQ_FATAL);
		FW->fatal = 0;
	}
	return 1;
}

void avdq_fw_idle(void)
{
	/* A masked pending IRQ wakes WFI; enable it only after the mode check. */
	__asm volatile("cpsid i" ::: "memory");
	if (!FW->mode)
		__asm volatile("dsb sy\n\twfi" ::: "memory");
	__asm volatile("cpsie i" ::: "memory");
}
