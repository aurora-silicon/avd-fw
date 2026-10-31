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
#define Q_DMA(off) REG(Q_PIODMA_BASE + (off))
#define SRAM_ALIAS 0x10000000u
#define Q_RING (SRAM_APERTURE + AVDQ_RING_OFFSET)
#define Q_DESC(i) ((volatile struct avdq_desc *)(SRAM_ALIAS + \
		   AVDQ_RING_OFFSET + (i) * AVDQ_DESC_BYTES))
#define Q_STAGE REG(SRAM_ALIAS + AVDQ_STAGE_OFFSET)
#define Q_INFO REG(SRAM_ALIAS + 0x7fc0u)

/* Explicit SRAM placement: no dependency on the legacy uninitialized BSS. */
struct queue_fw {
	struct avdq q;
	volatile unsigned int mode, fatal, in_write, in_read;
	volatile unsigned int inbox[AVDQ_COUNT];
	volatile unsigned int vp_done[12], pp_done, error;
	unsigned int dma_pending, dma_wait, stage_words, stage_used;
};
#define FW ((struct queue_fw *)(SRAM_ALIAS + 0x6000u))
_Static_assert(sizeof(struct queue_fw) <= 0x1fc0u, "SRAM queue state overlaps ABI");

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
	avdq_init(&FW->q);
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

static int feed(void *cookie, struct avdq_frame *f)
{
	struct queue_fw *fw = cookie;
	unsigned int words, max, cur;
	uint64_t addr;
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

static void kick(void *cookie, uint32_t word)
{
	(void)cookie;
	reg_write(REG(DECODE_CTRL_BASE + PP_FIFO), word);
}

static int complete(void *cookie, unsigned int idx, const struct avdq_frame *f)
{
	volatile struct avdq_desc *d = Q_DESC(idx);
	(void)cookie;
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
