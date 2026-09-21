// SPDX-License-Identifier: MIT

#include "irq.h"

#if AVD_VER == 5
#define PIODMA_CFG		REG(0x41070000)
#define PIODMA_STATUS		REG(0x41070004)
#define PIODMA_FIFO_STATUS	REG(0x4107000c)
#define PIODMA_TUNE_10		REG(0x41070010)
#define PIODMA_TUNE_18		REG(0x41070018)
#define PIODMA_TUNE_1C		REG(0x4107001c)
#define PIODMA_ADDR_LOW		REG(0x4107004c)
#define PIODMA_ADDR_HIGH	REG(0x41070050)
#define PIODMA_COMMAND		REG(0x41070054)

#define TANSY_CMD_BASE		0x01090000
#define TANSY_CMD_END		0x010a0000
#define TANSY_CMD_SIZE		0x60
#define TANSY_SRAM_ALIAS	0x0ef70000

#define TANSY_VP_CTRL		REG(0x411000b4)
#define TANSY_VP_IRQ_MASK	REG(0x4110015c)
#define TANSY_FIFO_IOVA_LOW(n)	REG(0x411001d0 + (n) * 0x3c)
#define TANSY_WRAP_CTRL		REG(0x41400000)
#define TANSY_VP0_FULL_IRQ	81
#define TANSY_HEVC_INST_FIFO	REG(0x4110000c)

#define PIODMA_HEADER_CMD	0x0000de11
#define PIODMA_WORK_CMD		0x00000c11
#define PIODMA_SLICE_CMD	0x00005c11
#define PIODMA_DONE		BIT(0)
#define TANSY_PROGRAM_WORDS	92
#define TANSY_HEADER_WORDS	78
#define TANSY_CONTINUATION_WORDS 91
#define TANSY_CONTINUATION_DST	REG(0x01080000 + TANSY_SRAM_ALIAS)
#define TANSY_NATIVE_CONTINUATION 0x4e415456

#define SCB_CFSR		REG(0xe000ed28)
#define SCB_HFSR		REG(0xe000ed2c)
#define SCB_MMFAR		REG(0xe000ed34)
#define SCB_BFAR		REG(0xe000ed38)

#define TANSY_CMD0		REG(TANSY_CMD_BASE + TANSY_SRAM_ALIAS + 0x2cf8)

static u32 *tansy_active_cmd[12];
static u32 tansy_active_frame[12];
static u32 tansy_full_count[12];
static u32 tansy_full_hold[12];
static u32 tansy_vp_state[12];
static u32 tansy_program[12][TANSY_PROGRAM_WORDS];
static u32 tansy_program_count[12];
static u32 tansy_piodma_status;

static int piodma_copy(u32 iova, u32 command)
{
	u32 timeout = 1000000;

	reg_write(PIODMA_ADDR_LOW, iova);
	reg_write(PIODMA_ADDR_HIGH, 0);
	reg_write(PIODMA_COMMAND, command);

	while (!(reg_read(PIODMA_STATUS) & PIODMA_DONE)) {
		if (!--timeout)
			return -1;
	}
	tansy_piodma_status = reg_read(PIODMA_STATUS);
	reg_write(PIODMA_STATUS, PIODMA_DONE);

	return 0;
}

static u32 tansy_vp_state_read(u32 n)
{
	u32 producer = reg_read(REG(0x41100044 + n * 0x3c));
	u32 consumer = reg_read(REG(0x4110007c + n * 0x3c));

	return (producer & 0xffff) | (consumer << 16);
}

static void tansy_vp_sync(u32 n)
{
	(void)tansy_vp_state_read(n);
}

/*
 * Apple's CM3 inserts six instruction slots between HEVC tail FIFO stores.
 * Keep the descriptor header path unthrottled; only the short post-PIODMA
 * command tail needs this cadence.
 */
static inline __attribute__((always_inline))
void tansy_hevc_tail_write(unsigned int value)
{
	*TANSY_HEVC_INST_FIFO = value;
	__asm__ volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop"
			 ::: "memory");
}

static inline __attribute__((always_inline))
void tansy_hevc_header_write(unsigned int value)
{
	*TANSY_HEVC_INST_FIFO = value;
	__asm__ volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop"
			 ::: "memory");
}

static inline void tansy_hevc_settle(unsigned int count)
{
	__asm__ volatile("1: nop\n\t"
			 "subs %0, #1\n\t"
			 "bne 1b"
			 : "+l" (count) : : "cc", "memory");
}

static int tansy_emit_hevc_first(u32 n, u32 *cmd)
{
	static const int header_offsets[TANSY_HEADER_WORDS] = {
		-1, 0x58, 0x5c, 0x60, 0x64, 0x34, 0x38, 0x3c,
		0x40, 0x68, 0x6c, 0x70, 0x74, 0x78, 0x7c, 0x80,
		0x84, 0xa4, -1, -1, -1, -1, 0xe0, 0x1ec,
		0xe4, 0x1f0, 0xe8, 0x1f4, 0xec, 0x1f8, 0xf0, 0x1fc,
		0xf4, 0x200, 0x10c, 0x218, 0x104, 0x210, 0x108, 0x214,
		-1, 0x110, 0x21c, 0x114, 0x220, 0x118, 0x224, 0x11c,
		0x228, 0xf8, 0x204, 0x1c0, -1, 0x1c8, 0x1c4, -1,
		0x1cc, 0x9c, 0xa0, -1, 0xfc, 0x208, 0x100, 0x20c,
		-1, -1, -1, -1, 0x120, 0x140, -1, 0x160,
		-1, 0x180, -1, 0x1a0, -1, -1,
	};
	u32 *frame = REG(cmd[7] + TANSY_SRAM_ALIAS - 0x34);
	u32 *work = REG(cmd[7] + TANSY_SRAM_ALIAS + 0xa14);
	unsigned int fifo = cmd[1] & 0xf;
	unsigned int size0 = work[5];
	unsigned int fifo_distance;
	unsigned int end_xy = work[3];
	unsigned int xor_hash = 0;
	unsigned int sum_hash = 0;
	unsigned int *program = (unsigned int *)tansy_program[n];
	unsigned int i;

	tansy_vp_state[n] = tansy_vp_state_read(n);
	fifo_distance = ((tansy_vp_state[n] & 0xff) -
			 ((tansy_vp_state[n] >> 16) & 0xff)) & 0xff;
	for (i = 0; i < TANSY_HEADER_WORDS; i++) {
		if (!i)
			program[i] = 0x2b000000 | (fifo << 4) | BIT(9);
		else if (header_offsets[i] < 0)
			program[i] = 0;
		else
			program[i] = *REG((u32)frame + header_offsets[i]);
		if (i == 1)
			program[i] -= BIT(16);
	}
	program[78] = 0x2d800000 | ((size0 >> 16) & 0x3ff);
	program[79] = work[6];
	program[80] = work[7];
	program[81] = 0x2c000000 | ((work[9] & 0xfff) << 12);
	for (i = 82; i < 87; i++)
		program[i] = 0;
	program[87] = 0x2a000000;
	program[88] = ((end_xy & 0xff) << 24) |
		      ((work[11] & 0xfff) << 12);
	program[89] = 0;
	program[90] = 0x01000000 | ((work[8] & 0xfff) << 12);
	program[91] = 0x2b000000 | (fifo << 4) | BIT(10);
	for (i = 0; i < TANSY_PROGRAM_WORDS; i++) {
		xor_hash ^= program[i];
		sum_hash += program[i];
	}
	cmd[19] = xor_hash;
	cmd[20] = sum_hash;
	cmd[21] = (unsigned int)program;

	i = 0;
	for (; i < 1; i++)
		tansy_hevc_header_write(program[i]);
	tansy_vp_sync(n);
	tansy_hevc_settle(62);
	for (; i < 20; i++)
		tansy_hevc_header_write(program[i]);
	tansy_vp_sync(n);
	tansy_hevc_settle(77);
	for (; i < 68; i++)
		tansy_hevc_header_write(program[i]);
	tansy_vp_sync(n);
	tansy_hevc_settle(39);
	for (; i < TANSY_HEADER_WORDS; i++)
		tansy_hevc_header_write(program[i]);
	tansy_vp_sync(n);
	tansy_hevc_settle(6);

	if (piodma_copy(tansy_active_frame[cmd[1] >> 16] + 0xd4c,
			PIODMA_SLICE_CMD))
		return -1;
	/* Apple uses 261 cycles when all 92 words fit, 205 when they do not. */
	tansy_hevc_settle(fifo_distance >= 88 ? 46 : 28);

	tansy_hevc_tail_write(0x2d800000 | ((size0 >> 16) & 0x3ff));
	tansy_hevc_tail_write(work[6]);
	tansy_hevc_tail_write(work[7]);
	tansy_hevc_tail_write(0x2c000000 | ((work[9] & 0xfff) << 12));
	tansy_hevc_tail_write(0);
	tansy_vp_sync(n);

	return 0;
}

static void tansy_emit_hevc_tail(u32 *cmd)
{
	u32 *work = REG(cmd[7] + TANSY_SRAM_ALIAS + 0xa14);
	unsigned int fifo = cmd[1] & 0xf;
	unsigned int end_xy = work[3];
	unsigned int i;

	for (i = 0; i < 4; i++)
		tansy_hevc_tail_write(0);
	tansy_hevc_tail_write(0x2a000000);
	tansy_hevc_tail_write(((end_xy & 0xff) << 24) |
			      ((work[11] & 0xfff) << 12));
	tansy_hevc_tail_write(0);
	tansy_hevc_tail_write(0x01000000 | ((work[8] & 0xfff) << 12));
	tansy_hevc_tail_write(0x2b000000 | (fifo << 4) | BIT(10));
}

/*
 * Tansy's HEVC path is descriptor-driven.  The AP places a 24-word command in
 * CM3 SRAM and rings mailbox 0.  PIODMA consumes the descriptor at the start
 * of each DART packet and copies the payload into the codec's SRAM queue.
 *
 * Keep this deliberately HEVC-only: the existing direct FIFO path remains in
 * use for H.264, VP9 and AV1.
 */
void irq1(void)
{
	u32 token = reg_read(CM3_MBOX0_RX);
	u32 *cmd;
	u32 *header;
	u32 work_iova;
	u32 work_count;
	u32 vp_slot;
	u32 i;

	/* Tansy clears mailbox-0 not-empty after consuming every AP command. */
	reg_write(CM3_MBOX_IRQCLR_0, CM3_MBOX0_NOT_EMPTY);

	if (token < TANSY_CMD_BASE || token > TANSY_CMD_END - TANSY_CMD_SIZE)
		return;

	cmd = REG(token + TANSY_SRAM_ALIAS);

	if ((cmd[0] & 0x1f) == 0) {
		/* Apple enables the wrapper before the INIT reset sequence too. */
		reg_write(TANSY_WRAP_CTRL, 3);
		avd_reset_decoder_blocks();
		/* Exact Tansy INIT interrupt routing state. */
		reg_write(CM3_MBOX_IRQEN(0), 0x3);
		reg_write(CM3_MBOX_IRQEN(1), 0x40000);
		reg_write(CM3_MBOX_IRQEN(2), 0);
		return;
	}
	if (cmd[0] == 4) {
		u32 offset;

		vp_slot = cmd[1] >> 16;
		offset = cmd[3] <= 5 ? (cmd[3] - 1) * 19 :
			 TANSY_HEADER_WORDS + (cmd[3] - 6) * 19;
		if (vp_slot >= 12 || !cmd[2] || cmd[2] > 19 || !cmd[3] ||
		    offset >= TANSY_PROGRAM_WORDS ||
		    cmd[2] > TANSY_PROGRAM_WORDS - offset) {
			cmd[23] = 0x5453f004;
			return;
		}
		for (i = 0; i < cmd[2]; i++)
			tansy_program[vp_slot][offset + i] = cmd[4 + i];
		if (tansy_program_count[vp_slot] < offset + cmd[2])
			tansy_program_count[vp_slot] = offset + cmd[2];
		cmd[23] = 0x54531000 | (cmd[3] & 0xfff);
		return;
	}
	if (cmd[0] == 3) {
		vp_slot = cmd[1] >> 16;
		if (vp_slot >= 12 || !tansy_active_frame[vp_slot] ||
		    tansy_program_count[vp_slot] != TANSY_PROGRAM_WORDS) {
			cmd[23] = 0x5453f003;
			return;
		}
		/* Match Apple's uninterrupted header -> PIODMA -> tail burst. */
		for (i = 0; i < TANSY_HEADER_WORDS; i++)
			reg_write(TANSY_HEVC_INST_FIFO, tansy_program[vp_slot][i]);
		if (cmd[5] == TANSY_NATIVE_CONTINUATION) {
			/*
			 * Native one-work packets carry a zero-filled 0x16c-byte
			 * continuation.  Reproduce that PIODMA result locally so the
			 * diagnostic path does not depend on DART1 source fetches.
			 */
			for (i = 0; i < TANSY_CONTINUATION_WORDS; i++)
				TANSY_CONTINUATION_DST[i] = 0;
		} else if (piodma_copy(tansy_active_frame[vp_slot] + 0xd4c,
				       PIODMA_SLICE_CMD)) {
			goto timeout;
		}
		i = TANSY_HEADER_WORDS;
		for (; i < TANSY_PROGRAM_WORDS; i++)
			reg_write(TANSY_HEVC_INST_FIFO, tansy_program[vp_slot][i]);
		tansy_full_hold[vp_slot] = 0;
		cmd[23] = 0x54530007;
		/* Apple leaves FULL masked after installing the tail program. */
		reg_write(TANSY_VP_IRQ_MASK, 0x7);
		reg_write(CM3_MBOX_IRQEN(2), 0x7);
		return;
	}
	if (cmd[0] == 2) {
		vp_slot = cmd[1] >> 16;
		if (vp_slot >= 12 || !tansy_active_frame[vp_slot] ||
		    tansy_program_count[vp_slot] < TANSY_HEADER_WORDS)
			return;
		cmd[23] = 0x54530005;
		/* Command 3 emits the buffered program and continuation atomically. */
		cmd[23] = 0x54530006;
		return;
	}

	/* DECODE with mode zero is HEVC. */
	if (cmd[0] != 1)
		return;
	/* Apple enables the Tansy wrapper before touching decoder reset state. */
	reg_write(TANSY_WRAP_CTRL, 3);
	avd_reset_decoder_blocks();

	work_count = cmd[5];
	if (!work_count || work_count > 0x1000)
		return;
	vp_slot = cmd[1] >> 16;
	if (vp_slot >= 12)
		return;
	tansy_active_cmd[vp_slot] = cmd;
	tansy_active_frame[vp_slot] = cmd[2];
	tansy_full_count[vp_slot] = 0;
	tansy_vp_state[vp_slot] = 0;
	tansy_program_count[vp_slot] = 0;
	/* cmd[6] is an open-driver-only diagnostic request for host FIFO data. */
	tansy_full_hold[vp_slot] = cmd[6] != 0;
	cmd[6] = 0;
	cmd[23] = 0x54530001;
	header = REG(cmd[7] + TANSY_SRAM_ALIAS - 0x34);
	cmd[8] = header[0];
	cmd[9] = header[1];
	cmd[10] = reg_read(PIODMA_STATUS);

	if (piodma_copy(cmd[2], PIODMA_HEADER_CMD))
		goto timeout;
	cmd[11] = header[0];
	cmd[13] = header[1];
	cmd[14] = tansy_piodma_status;
	cmd[15] = reg_read(PIODMA_STATUS);
	/* Match Apple's 317-cycle header-to-work PIODMA interval. */
	tansy_hevc_settle(44);
	cmd[23] = 0x54530002;

	for (i = 0; i < 5; i++)
		reg_write(TANSY_FIFO_IOVA_LOW(i), 0);
	reg_write(TANSY_VP_CTRL, 0);
	reg_write(TANSY_VP_IRQ_MASK, 7);

	work_iova = cmd[12];
	for (i = 0; i < work_count; i++) {
		if (piodma_copy(work_iova + i * 0x30, PIODMA_WORK_CMD))
			goto timeout;
	}
	/* Match Apple's 227-cycle interval before the duplicate work copy. */
	tansy_hevc_settle(44);
	cmd[23] = 0x54530003;

	/* Exact Tansy DECODE routing: VP0 unknown/error/done/full. */
	reg_write(CM3_MBOX_IRQEN(2), 0xf);
	reg_write(TANSY_VP_IRQ_MASK, 0xf);
	for (i = 0; i < work_count; i++) {
		if (piodma_copy(work_iova + i * 0x30, PIODMA_WORK_CMD))
			goto timeout;
	}
	/* Return live v5 PIODMA state after every source transfer completed. */
	cmd[12] = reg_read(PIODMA_CFG);
	cmd[16] = reg_read(PIODMA_TUNE_10);
	cmd[17] = reg_read(PIODMA_TUNE_18);
	cmd[18] = reg_read(PIODMA_TUNE_1C);
	cmd[22] = reg_read(PIODMA_FIFO_STATUS);
	cmd[23] = 0x54530004;

	return;

timeout:
	cmd[22] = reg_read(PIODMA_STATUS);
	cmd[23] = 0x5453ffff;
	reg_write(CM3_MBOX1_TX, 0x10000 | 0x7ffe);
}
#endif

void irq_handler(void)
{
	u32 ipsr;
	u32 submit;
	u32 irq_num;

	__asm volatile("mrs %0, ipsr" : "=r"(ipsr));
	irq_num = ipsr & 0x1ff;

	submit = 0x10000 | (irq_num < 16 ? 1000 + irq_num : irq_num - 16);

	reg_write(CM3_MBOX1_TX, submit);

	while (1)
	__asm volatile("wfi");
}

void irq_nmi(void)
{
	irq_handler();
}

void irq_hardfault(void)
{
#if AVD_VER == 5
	u32 *cmd = TANSY_CMD0;

	cmd[18] = reg_read(SCB_CFSR);
	cmd[19] = reg_read(SCB_HFSR);
	cmd[20] = reg_read(SCB_MMFAR);
	cmd[21] = reg_read(SCB_BFAR);
	cmd[23] = 0x5453f003;
	reg_write(CM3_MBOX1_TX, 0x10000 | 0x7ffd);

	while (1)
		__asm volatile("wfi");
#else
	irq_handler();
#endif
}

void irq_memmanage(void)
{
	irq_handler();
}

void irq_busfault(void)
{
	irq_handler();
}

void irq_usagefault(void)
{
	irq_handler();
}

void irq_svcall(void)
{
	irq_handler();
}

void irq_pendsv(void)
{
	irq_handler();
}

void irq_systick(void)
{
	irq_handler();
}

void irq_clear(u32 n, u32 status)
{
	volatile u32 *reg = DECODE_STATUS(n);
#if AVD_VER == 2
	/* packed into one register each status is shifted 5 up */
	reg_write(reg, status << (n * 5));
	while (reg_read(reg) & (status << (n * 5)));
#else
	reg_write(reg, status);
	while (reg_read(reg) & status);
#endif
}

static void vpdone(u32 n)
{
#if AVD_VER == 5
	if (n == 0)
		TANSY_CMD0[16]++;
#endif
	irq_clear(n, DECODE_STATUS_DONE);
	reg_write(CM3_MBOX1_TX, 0x100 | n);
}

static void err(u32 n)
{
	irq_clear(n, DECODE_STATUS_ERR);
	reg_write(CM3_MBOX1_TX, n);
}

static void ppdone(u32 n)
{
	irq_clear(n, DECODE_STATUS_DONE);
	reg_write(CM3_MBOX1_TX, 0x1000);
}

#if AVD_VER == 5
static void full(u32 n)
{
	u32 *mask = REG(0x4110015c + n * sizeof(u32));
	u32 *cmd = tansy_active_cmd[n];

	tansy_full_count[n]++;
	if (cmd)
		cmd[6] = (tansy_full_hold[n] ? BIT(31) : 0) |
			 tansy_full_count[n];
	/*
	 * Apple's handler rearms this level source by toggling only the VP mask.
	 * Our AP-command bridge holds it masked while the host installs the header,
	 * continuation PIODMA, and tail program.  DONE/error remain enabled.
	 */
	if (tansy_full_hold[n]) {
		if (n == 0)
			reg_write(CM3_MBOX_IRQEN(2), 7);
		reg_write(mask, reg_read(mask) & ~DECODE_STATUS_FULL);
		(void)reg_read(DECODE_STATUS(n));
		reg_write(DECODE_STATUS(n), DECODE_STATUS_FULL);
		if (cmd && tansy_full_count[n] == 1)
			cmd[23] = 0x54530005;
	} else {
		if (n == 0)
			reg_write(CM3_MBOX_IRQEN(2), 7);
		reg_write(mask, reg_read(mask) & ~DECODE_STATUS_FULL);
		(void)reg_read(DECODE_STATUS(n));
		reg_write(DECODE_STATUS(n), DECODE_STATUS_FULL);
		if (cmd && tansy_full_count[n] == 1) {
			if (tansy_emit_hevc_first(n, cmd)) {
				cmd[22] = reg_read(PIODMA_STATUS);
				cmd[23] = 0x5453ff81;
				reg_write(CM3_MBOX1_TX, 0x10000 | 0x7ffe);
			} else if ((((tansy_vp_state[n] & 0xff) -
				      ((tansy_vp_state[n] >> 16) & 0xff)) &
				     0xff) >= 88) {
				/* Apple has room for the final nine words immediately. */
				/* Its first and second tail groups are 241 cycles apart. */
				tansy_hevc_settle(64);
				tansy_emit_hevc_tail(cmd);
				cmd[23] = 0x54530007;
			} else {
				cmd[23] = 0x54530008;
				/* Apple rearms FULL until the remaining nine words fit. */
				reg_write(mask, reg_read(mask) |
					  DECODE_STATUS_FULL);
				if (n == 0)
					reg_write(CM3_MBOX_IRQEN(2), 0xf);
			}
		} else if (cmd && tansy_full_count[n] >= 2 &&
			   cmd[23] == 0x54530008) {
			u32 state = tansy_vp_state_read(n);

			if (state == tansy_vp_state[n]) {
				/* A level reassert can arrive before the FIFO advances. */
				reg_write(mask, reg_read(mask) |
					  DECODE_STATUS_FULL);
				if (n == 0)
					reg_write(CM3_MBOX_IRQEN(2), 0xf);
			} else {
				tansy_vp_state[n] = state;
				tansy_emit_hevc_tail(cmd);
				cmd[23] = 0x54530007;
			}
		}
	}
}

#define irq_full(n, idx) irq(n) { full(idx); }

irq_full(81, 0)
irq_full(86, 1)
irq_full(91, 2)
irq_full(96, 3)
irq_full(101, 4)
irq_full(106, 5)
irq_full(111, 6)
irq_full(116, 7)
irq_full(121, 8)
irq_full(126, 9)
irq_full(131, 10)
irq_full(136, 11)
#endif

/* version 2? */
void irq38(void)
{
	irq_clear(IRQ_SUBMIT, DECODE_STATUS_UNK);
}

void irq40(void)
{
	ppdone(IRQ_SUBMIT);
}

/* version 3+ */
void irq62(void)
{
	irq_clear(IRQ_SUBMIT, DECODE_STATUS_UNK);
}

void irq64(void)
{
	ppdone(IRQ_SUBMIT);
}

irq_unk(18, 0)
irq_err(19, 0)
irq_vdone(20, 0)

irq_unk(23, 1)
irq_err(24, 1)
irq_vdone(25, 1)

irq_unk(28, 2)
irq_err(29, 2)
irq_vdone(30, 2)

irq_unk(33, 3)
irq_err(34, 3)
irq_vdone(35, 3)

irq_unk(78, 0)
irq_err(79, 0)
irq_vdone(80, 0)

irq_unk(83, 1)
irq_err(84, 1)
irq_vdone(85, 1)

irq_unk(88, 2)
irq_err(89, 2)
irq_vdone(90, 2)

irq_unk(93, 3)
irq_err(94, 3)
irq_vdone(95, 3)

irq_unk(98, 4)
irq_err(99, 4)
irq_vdone(100, 4)

irq_unk(103, 5)
irq_err(104, 5)
irq_vdone(105, 5)

irq_unk(108, 6)
irq_err(109, 6)
irq_vdone(110, 6)

irq_unk(113, 7)
irq_err(114, 7)
irq_vdone(115, 7)

irq_unk(118, 8)
irq_err(119, 8)
irq_vdone(120, 8)

irq_unk(123, 9)
irq_err(124, 9)
irq_vdone(125, 9)

irq_unk(128, 10)
irq_err(129, 10)
irq_vdone(130, 10)

irq_unk(133, 11)
irq_err(134, 11)
irq_vdone(135, 11)

#if AVD_VER == 5
IRQ(0) IRQ(2) IRQ(3)
#else
IRQ(0) IRQ(1) IRQ(2) IRQ(3)
#endif

IRQ(4) IRQ(5) IRQ(6) IRQ(7)
IRQ(8) IRQ(9) IRQ(10) IRQ(11)
IRQ(12) IRQ(13) IRQ(14) IRQ(15)
IRQ(16) IRQ(17)

IRQ(21) IRQ(22) IRQ(26) IRQ(27)
IRQ(31) IRQ(32) IRQ(36) IRQ(37)
IRQ(39) IRQ(41)

IRQ(42) IRQ(43) IRQ(44) IRQ(45)
IRQ(46) IRQ(47) IRQ(48) IRQ(49)
IRQ(50) IRQ(51) IRQ(52) IRQ(53)
IRQ(54) IRQ(55) IRQ(56) IRQ(57)
IRQ(58) IRQ(59) IRQ(60) IRQ(61)

IRQ(63) IRQ(65)

IRQ(66) IRQ(67) IRQ(68) IRQ(69)
IRQ(70) IRQ(71) IRQ(72) IRQ(73)
IRQ(74) IRQ(75) IRQ(76) IRQ(77)

#if AVD_VER == 5
IRQ(82) IRQ(87) IRQ(92) IRQ(97)
IRQ(102) IRQ(107) IRQ(112) IRQ(117)
IRQ(122) IRQ(127) IRQ(132) IRQ(137)
#else
IRQ(81) IRQ(82) IRQ(86) IRQ(87)
IRQ(91) IRQ(92) IRQ(96) IRQ(97)
IRQ(101) IRQ(102) IRQ(106) IRQ(107)
IRQ(111) IRQ(112) IRQ(116) IRQ(117)
IRQ(121) IRQ(122)
IRQ(126) IRQ(127) IRQ(131) IRQ(132)
IRQ(136) IRQ(137)
#endif
IRQ(138) IRQ(139)
IRQ(140)
