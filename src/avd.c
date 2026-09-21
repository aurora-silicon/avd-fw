// SPDX-License-Identifier: MIT

#include "avd.h"

extern void _start(void);

__attribute__((section(".vectors"), used))
static void (*const vector_table[])(void) = {
	(void *)SP,
	_start,
	irq_nmi,
	irq_hardfault,
	irq_memmanage,
	irq_busfault,
	irq_usagefault,
	0,
	0,
	0,
	0,
	irq_svcall,
	0,
	0,
	irq_pendsv,
	irq_systick,

	irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7, irq8, irq9, irq10, irq11,
	irq12, irq13, irq14, irq15, irq16, irq17, irq18, irq19, irq20, irq21,
	irq22, irq23, irq24, irq25, irq26, irq27, irq28, irq29, irq30, irq31,
	irq32, irq33, irq34, irq35, irq36, irq37, irq38, irq39, irq40, irq41,
	irq42, irq43, irq44, irq45, irq46, irq47, irq48, irq49, irq50, irq51,
	irq52, irq53, irq54, irq55, irq56, irq57, irq58, irq59, irq60, irq61,
	irq62, irq63, irq64, irq65, irq66, irq67, irq68, irq69, irq70, irq71,
	irq72, irq73, irq74, irq75, irq76, irq77, irq78, irq79, irq80, irq81,
	irq82, irq83, irq84, irq85, irq86, irq87, irq88, irq89, irq90, irq91,
	irq92, irq93, irq94, irq95, irq96, irq97, irq98, irq99, irq100, irq101,
	irq102, irq103, irq104, irq105, irq106, irq107, irq108, irq109, irq110,
	irq111, irq112, irq113, irq114, irq115, irq116, irq117, irq118, irq119,
	irq120, irq121, irq122, irq123, irq124, irq125, irq126, irq127, irq128,
	irq129, irq130, irq131, irq132, irq133, irq134, irq135, irq136, irq137,
	irq138, irq139, irq140
};

void apply_tunables()
{
	int n_tunables, i = 0;

	n_tunables = sizeof(avd_tunables) / sizeof(struct tunable);

	for (i = 0; i < n_tunables; i++) {
		u32 val, old_val;

		u32 *reg = REG(DECODE_CTRL_BASE + avd_tunables[i].off);

		old_val = reg_read(reg);
		val = old_val & ~avd_tunables[i].mask;
		val |= avd_tunables[i].val;
		/* always write */
		reg_write(reg, val);
	}
}

#if AVD_VER == 5
#define PIODMA_BASE 0x41070000

static const struct tunable piodma_tunables[] = {
	{0x00, 0x00000002, 0x00000000},
	{0x10, 0x1fffffff, 0x0001fbf2},
	{0x14, 0x1fffffff, 0x0001fbf2},
	{0x18, 0x1fffffff, 0x0001fbf3},
	{0x1c, 0x0007ffff, 0x0001fbf0},
	{0x20, 0x0007ffff, 0x0001fbf0},
};

/*
 * Hibiscus' AppleAVD avd_piodma table programs these six records before the
 * CM3 firmware runs.  Linux has no host-side AppleAVD kext to do that work,
 * so initialize the v5 engine here.  These are masked tunables only; the
 * aperture-bank setup used by T8140 PCIe PIODMA is not part of AVD's table.
 */
static void apply_piodma_tunables(void)
{
	int n_tunables = sizeof(piodma_tunables) / sizeof(struct tunable);
	u32 *cfg = REG(PIODMA_BASE);
	int i;

	for (i = 0; i < n_tunables; i++) {
		u32 *reg = REG(PIODMA_BASE + piodma_tunables[i].off);
		u32 value = reg_read(reg);

		value &= ~piodma_tunables[i].mask;
		value |= piodma_tunables[i].val;
		reg_write(reg, value);
	}

	/* CFG bit 0 is the engine stop gate; Apple's wrapper clears it. */
	reg_write(cfg, reg_read(cfg) & ~BIT(0));
}

static void reset_tunable_range(u32 first, u32 last)
{
	int n_tunables = sizeof(avd_tunables) / sizeof(struct tunable);
	int i;

	for (i = 0; i < n_tunables; i++) {
		u32 off = avd_tunables[i].off;

		if (off < first || off > last)
			continue;
		if (!(avd_tunables[i].mask & BIT(31)))
			continue;
		reg_write(REG(DECODE_CTRL_BASE + off), 0xc0000000);
	}
}

/*
 * Apple's Tansy firmware runs this reset/enable sequence for both INIT and
 * DECODE.  Loading the kext tunables at boot is not a replacement: the
 * 0xc0000000 writes are command strobes which arm every decoder sub-block.
 * Keep the observed ordering, including the early E8 group, since several
 * blocks share reset and clock-gating dependencies.
 */
void avd_reset_decoder_blocks(void)
{
	int i;

	reg_write(REG(DECODE_CTRL_BASE + 0x0008), 0);
	reg_write(REG(DECODE_CTRL_BASE + 0x0190), 0);
	reg_write(REG(DECODE_CTRL_BASE + 0x03cc), 0);
	for (i = 0; i < 7; i++)
		reg_write(REG(DECODE_CTRL_BASE + 0x037c + i * 4), 1);

	reset_tunable_range(0x1000, 0x1900);
	reg_write(REG(DECODE_CTRL_BASE + 0x0008), 0xc0000000);
	reset_tunable_range(0x4000, 0x4700);

	reg_write(REG(DECODE_CTRL_BASE + 0xc000), 1);
	reg_write(REG(DECODE_CTRL_BASE + 0xc02c), 1);
	reg_write(REG(DECODE_CTRL_BASE + 0xe000), 1);
	reg_write(REG(DECODE_CTRL_BASE + 0xe800), 1);

	reset_tunable_range(0xc080, 0xd540);
	reset_tunable_range(0xe880, 0xe940);
	reset_tunable_range(0xe080, 0xe700);

	reg_write(REG(DECODE_CTRL_BASE + 0xe980), 0x80000003);
	reset_tunable_range(0xe740, 0xe740);
	reg_write(REG(DECODE_CTRL_BASE + 0x0190), 5);
}
#endif

void _start(void)
{
	/* Tansy explicitly restores the architectural vector base on each boot. */
	reg_write(CM3_SCB_VTOR, 0);
	apply_tunables();
#if AVD_VER == 5
	apply_piodma_tunables();
#endif
	/* Match Tansy's mailbox-0 doorbell enables (not-empty plus channel 5). */
	reg_write(CM3_MBOX_IRQEN_0, 0x22);

#if AVD_VER == 5 && AVD_TIER == 0
	/*
	 * Tansy leaves reserved CM3 interrupt lines disabled.  Enabling every
	 * line can take a latent reserved interrupt into the catch-all handler
	 * before the mailbox doorbell is serviced.  These are the exact ISER
	 * masks programmed by Apple's Tansy firmware at boot.
	 */
	static const u32 tansy_nvic_iser[] = {
		0x0700c026, 0xc0000000, 0xef7bc00b, 0x7bdef7bd,
		0x000001ef, 0x00000000, 0x00000000,
	};

	for (int i = 0; i < 7; i++)
		CM3_NVIC_ISER[i] = tansy_nvic_iser[i];
#else
	for (int i = 0; i < 7; i++)
		CM3_NVIC_ISER[i] = 0xffffffff;
#endif

	__asm volatile("cpsie i");

	reg_write(CM3_BOOT, 1);

	while (1)
		__asm volatile("wfi");
}
