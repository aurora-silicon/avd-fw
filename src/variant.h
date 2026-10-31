/* SPDX-License-Identifier: MIT */
#ifndef AVD_FW_VARIANT_H
#define AVD_FW_VARIANT_H
#include <stdint.h>

#define AVDQ_VPS 12u
struct avdq_variant {
	uint8_t base[4], count[4];
	uint8_t fifo_slots, fifo_mask, exec_start, lsr;
};
/* Public codec-specific VP register indices, including tier-0 holes. */
static const struct avdq_variant avdq_variants[] = {
	{ {0, 2, 3, 0}, {2, 1, 1, 0}, 7, 7, 8, 1 }, /* v2-t0 */
	{ {0, 4, 8, 9}, {4, 4, 1, 0}, 15, 15, 9, 1 }, /* v3-t0 */
	{ {0, 4, 8, 9}, {4, 4, 1, 0}, 15, 15, 9, 1 }, /* v3-t1 */
	{ {0, 4, 8, 9}, {4, 4, 1, 0}, 15, 15, 9, 0 }, /* v3-t2 */
	{ {0, 4, 8, 9}, {4, 4, 1, 2}, 15, 15, 9, 0 }, /* v4-t0 */
	{ {0, 4, 8, 9}, {2, 1, 1, 1}, 7, 7, 9, 0 }, /* v5-t0 */
	{ {0, 4, 8, 9}, {4, 4, 1, 3}, 15, 15, 9, 0 }, /* v5-t1 */
	{ {0, 4, 8, 9}, {4, 4, 1, 3}, 15, 15, 9, 0 }, /* v5-t2, inferred */
};
#if defined(AVD_VER)
#if AVD_VER == 2
#define AVDQ_VARIANT 0
#elif AVD_VER == 3
#define AVDQ_VARIANT (1 + AVD_TIER)
#elif AVD_VER == 4
#define AVDQ_VARIANT 4
#else
#define AVDQ_VARIANT (5 + AVD_TIER)
#endif
#define AVDQ_IMAGE_VARIANT (&avdq_variants[AVDQ_VARIANT])
#endif
#endif
