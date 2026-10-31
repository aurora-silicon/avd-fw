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
#define Q_MULTI_INFO REG(SRAM_ALIAS + AVDQ_MULTI_INFO_OFFSET)

/* Explicit SRAM placement: no dependency on the legacy uninitialized BSS. */
struct queue_fw {
	struct avdq q;
	volatile unsigned int mode, fatal, in_write, in_read;
	volatile unsigned int inbox[AVDQ_COUNT];
	volatile unsigned int vp_done[12], pp_done, error, vp_ready, fifo_wait;
	unsigned int runnable;
	unsigned int dma_pending, dma_wait, stage_words, stage_used;
};
#define FW ((struct queue_fw *)(SRAM_ALIAS + 0x6000u))
_Static_assert(sizeof(struct queue_fw) <= 0x1f80u, "SRAM queue state overlaps ABI");

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
	avdq_init(&FW->q, AVDQ_IMAGE_VARIANT);
	for (i = 0; i < 16; i++) {
		Q_INFO[i] = 0;
		Q_MULTI_INFO[i] = 0;
	}
	Q_INFO[1] = AVDQ_VERSION;
	Q_INFO[2] = AVDQ_COUNT;
	Q_INFO[3] = AVDQ_DESC_BYTES;
	Q_INFO[4] = SRAM_APERTURE;
	Q_INFO[5] = AVDQ_RING_OFFSET;
	Q_INFO[6] = AVDQ_PACKET_WORDS;
	Q_INFO[7] = avdq_packet_header(SRAM_APERTURE + AVDQ_STAGE_OFFSET);
	Q_INFO[8] = VP_COUNT;
	Q_INFO[9] = (AVD_VER <= 3 ? 7u : 15u) | AVDQ_CAP_MULTI_VP;
	Q_INFO[10] = FIFO_SLOTS;
	for (i = 0; i < 4; i++)
		Q_MULTI_INFO[2 + i] = AVDQ_IMAGE_VARIANT->base[i] |
			(AVDQ_IMAGE_VARIANT->count[i] << 8);
	Q_MULTI_INFO[1] = AVDQ_MULTI_VERSION;
	Q_MULTI_INFO[6] = AVDQ_FLAG_MULTI_VP;
	barrier();
	Q_MULTI_INFO[0] = AVDQ_MULTI_MAGIC;
	barrier();
	Q_INFO[0] = AVDQ_MAGIC;
}

int avdq_fw_mailbox(unsigned int token)
{
	int idx = avdq_index(token, Q_RING);
	unsigned int write;
	if (idx < 0)
		return FW->mode != 0; /* Never run Tansy resets during a queue session. */
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
	if (event == DECODE_STATUS_FULL) {
		if (vp < VP_COUNT && (FW->fifo_wait & (1u << vp)))
			FW->vp_ready |= 1u << vp;
		return 1;
	}
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

void avdq_fw_full_mask(unsigned int vp, unsigned int enable)
{
#if AVD_VER == 2
	u32 *mask = REG(DECODE_CTRL_BASE + 0x405c);
	u32 bit = DECODE_STATUS_FULL << (vp * 5);
#elif AVD_VER == 3
	u32 *mask = REG(DECODE_CTRL_BASE + 0xfc + vp * 4);
	u32 bit = DECODE_STATUS_FULL;
#else
	u32 *mask = REG(DECODE_CTRL_BASE + 0x15c + vp * 4);
	u32 bit = DECODE_STATUS_FULL;
#endif
	reg_write(mask, enable ? reg_read(mask) | bit : reg_read(mask) & ~bit);
}

static void configure_fifo(const struct avdq_frame *f)
{
	u32 fifo = f->work.fifo * 4u;
	uint64_t addr = ((uint64_t)f->work.fifo_hi << 32) | f->work.fifo_lo;
#if AVD_VER == 2
	if (f->work.extended) {
		reg_write(REG(DECODE_CTRL_BASE + 0x4068 + fifo), (u32)(addr >> 8));
		reg_write(REG(DECODE_CTRL_BASE + 0x4084 + fifo), 0);
		reg_write(REG(DECODE_CTRL_BASE + 0x40a0 + fifo), 0);
		reg_write(REG(DECODE_CTRL_BASE + 0x40bc + fifo), 0);
	}
	reg_write(REG(DECODE_CTRL_BASE + 0x4040 + f->vp * 4), 0);
	u32 *mask = REG(DECODE_CTRL_BASE + 0x405c);
	reg_write(mask, reg_read(mask) | (7u << (f->vp * 5)) | (5u << 20));
#else
#if AVD_VER == 3
	const u32 lo = 0x150, hi = 0x30c, mask = 0x18c, cach = 0x1c8, xfer = 0x204;
	const u32 vp_irq = 0xfc, pp_irq = 0x120;
#else
	const u32 lo = 0x1d0, hi = 0x20c, mask = 0x248, cach = 0x284, xfer = 0x2c0;
	const u32 vp_irq = 0x15c, pp_irq = 0x190;
#endif
	if (f->work.extended) {
		if (AVDQ_IMAGE_VARIANT->lsr)
			reg_write(REG(DECODE_CTRL_BASE + lo + fifo), (u32)(addr >> 8));
		else {
			reg_write(REG(DECODE_CTRL_BASE + hi + fifo), (u32)(addr >> 32));
			reg_write(REG(DECODE_CTRL_BASE + lo + fifo), (u32)addr);
		}
		reg_write(REG(DECODE_CTRL_BASE + mask + fifo), 0);
		reg_write(REG(DECODE_CTRL_BASE + cach + fifo), 0);
		reg_write(REG(DECODE_CTRL_BASE + xfer + fifo), 0);
	}
	u32 *vp_mask = REG(DECODE_CTRL_BASE + vp_irq + f->vp * 4);
	u32 *pp_mask = REG(DECODE_CTRL_BASE + pp_irq);
	reg_write(vp_mask, reg_read(vp_mask) | 7u);
	reg_write(pp_mask, reg_read(pp_mask) | 5u);
#endif
}

static int feed(void *cookie, struct avdq_frame *f)
{
	struct queue_fw *fw = cookie;
	unsigned int words, max, cur;
	unsigned int bit = 1u << f->vp;
	uint64_t addr;
	/* FULL is a not-full notification. No occupancy reads while asleep. */
	if (fw->fifo_wait & bit) {
		__asm volatile("cpsid i" ::: "memory");
		if (!(fw->vp_ready & bit)) {
			__asm volatile("cpsie i" ::: "memory");
			return 0;
		}
		fw->vp_ready &= ~bit;
		fw->fifo_wait &= ~bit;
		__asm volatile("cpsie i" ::: "memory");
	}
	if (fw->stage_used == fw->stage_words) {
		if (!fw->dma_pending) {
			if (!f->pushed)
				configure_fifo(f);
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
			fw->runnable = 1;
			fw->dma_pending = 1;
			fw->dma_wait = 1000000;
			return 0;
		}
		if (!(reg_read(Q_DMA(0x04)) & 1u)) {
			fw->runnable = 1;
			return --fw->dma_wait ? 0 : -1;
		}
		reg_write(Q_DMA(0x04), 1);
		barrier();
		fw->stage_words = f->desc.program_words - f->pushed;
		if (fw->stage_words > AVDQ_PACKET_WORDS)
			fw->stage_words = AVDQ_PACKET_WORDS;
		fw->stage_used = fw->dma_pending = 0;
	}
	/* Same per-VP capacity/occupancy registers as Linux's direct FIFO path. */
	max = reg_read(REG(DECODE_CTRL_BASE + FIFO_MAX + f->vp * 4u));
	cur = reg_read(REG(DECODE_CTRL_BASE + FIFO_CUR + f->vp * 4u));
	if (!max)
		return -1;
	if (cur >= max) {
		/* Arm then recheck to close the drain-before-unmask race. */
		__asm volatile("cpsid i" ::: "memory");
		fw->vp_ready &= ~bit;
		fw->fifo_wait |= bit;
		avdq_fw_full_mask(f->vp, 1);
		cur = reg_read(REG(DECODE_CTRL_BASE + FIFO_CUR + f->vp * 4u));
		if (cur < max) {
			avdq_fw_full_mask(f->vp, 0);
			fw->fifo_wait &= ~bit;
			fw->runnable = 1;
		}
		__asm volatile("cpsie i" ::: "memory");
		return 0;
	}
	reg_write(REG(DECODE_CTRL_BASE + VP_FIFO + f->vp * 4u),
		  Q_STAGE[fw->stage_used++]);
	/* Conservative local store cadence also used by the Tansy HEVC path. */
	__asm volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop" ::: "memory");
	fw->runnable = 1;
	f->pushed++;
	if (f->pushed == f->desc.program_words) {
		fw->stage_words = fw->stage_used = 0;
		return 1;
	}
	return 0;
}

static void kick(void *cookie, uint32_t word)
{
	((struct queue_fw *)cookie)->runnable = 1;
	reg_write(REG(DECODE_CTRL_BASE + PP_FIFO), word);
}

static int complete(void *cookie, unsigned int idx, const struct avdq_frame *f)
{
	volatile struct avdq_desc *d = Q_DESC(idx);
	((struct queue_fw *)cookie)->runnable = 1;
	if (!(reg_read(CM3_MBOX1_STATUS) & CM3_MBOX1_EMPTY))
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
	FW->runnable = 0;
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
		FW->runnable = 1;
		if (avdq_submit(&FW->q, idx, &d)) {
			FW->fatal = 1;
			avdq_fault(&FW->q);
		}
		/* Queue mode owns PIODMA; the Tansy transport cannot run concurrently. */
		reg_write(Q_DMA(0x00), reg_read(Q_DMA(0x00)) & ~1u);
	}
	avdq_step(&FW->q, &ops, FW);
	if (FW->fatal && !FW->q.count &&
	    (reg_read(CM3_MBOX1_STATUS) & CM3_MBOX1_EMPTY)) {
		reg_write(CM3_MBOX1_TX, AVDQ_FATAL);
		FW->fatal = 0;
	}
	return FW->runnable;
}

void avdq_fw_idle(void)
{
	/* Check latched events with IRQs masked to close the idle-entry race. */
	__asm volatile("cpsid i" ::: "memory");
	unsigned int events = FW->error | FW->pp_done | FW->vp_ready;
	for (unsigned int i = 0; i < VP_COUNT; i++)
		events |= FW->vp_done[i];
	if (!events && FW->in_read == FW->in_write && !FW->fatal)
		__asm volatile("dsb sy\n\twfi" ::: "memory");
	__asm volatile("cpsie i" ::: "memory");
}
