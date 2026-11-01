/* SPDX-License-Identifier: MIT */
#ifndef AVD_FW_POOL_H
#define AVD_FW_POOL_H
#include <stdint.h>

#define AVDQ_POOL_OFFSET 0x7f80u
#define AVDQ_POOL_MAGIC 0x41564d32u /* AVM2, distinct from the earlier AVM1 ABI */
#define AVDQ_POOL_VERSION 1u

struct avdq_pool {
	uint32_t codec[4]; /* bit n selects hardware VP register index n */
	uint32_t inferred; /* bit n: codec n's count is inferred */
};

/* Public codec pools, ported from avd/multi-vp's MIT variant table. */
static const struct avdq_pool avdq_pools[] = {
	{ {0x003, 0x004, 0x008, 0x000}, 0 }, /* v2-t0, T8103 */
	{ {0x00f, 0x0f0, 0x100, 0x000}, 0 }, /* v3-t0, T6000 */
	{ {0x00f, 0x0f0, 0x100, 0x000}, 0 }, /* v3-t1, T8112 */
	{ {0x00f, 0x0f0, 0x100, 0x000}, 0 }, /* v3-t2, T6020 */
	{ {0x00f, 0x0f0, 0x100, 0x600}, 0 }, /* v4-t0, T8122 */
	{ {0x003, 0x010, 0x100, 0x200}, 0 }, /* v5-t0, T8140 */
	{ {0x00f, 0x0f0, 0x100, 0xe00}, 0 }, /* v5-t1, T8132 */
	{ {0x00f, 0x0f0, 0x100, 0xe00}, 15 }, /* v5-t2, T6040 inferred */
};

#if defined(AVD_VER)
#if AVD_VER == 2
#define AVDQ_IMAGE_POOL (&avdq_pools[0])
#elif AVD_VER == 3
#define AVDQ_IMAGE_POOL (&avdq_pools[1 + AVD_TIER])
#elif AVD_VER == 4
#define AVDQ_IMAGE_POOL (&avdq_pools[4])
#else
#define AVDQ_IMAGE_POOL (&avdq_pools[5 + AVD_TIER])
#endif
#endif
#endif
