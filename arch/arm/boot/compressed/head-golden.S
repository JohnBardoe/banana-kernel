/* SPDX-License-Identifier: GPL-2.0 */
/*
 * linux/arch/arm/boot/compressed/head-golden.S
 *
 * Golden specific tweaks.  This is merged into head.S by the linker.
 *
 */

#include <linux/linkage.h>

/* for l2cc enable and clean_invalidate */
#define L2X0_CTRL		0x100
#define L2X0_CLEAN_INV_WAY	0x7FC
#define L2X0_WAY_MASK		0xFFFF
#define L2X0_DATA_LOCKDOWN_WAY	0x900
#define L2X0_INST_LOCKDOWN_WAY	0x904

		.section        ".start", "ax"

__golden_start:

	@ Preserve r8/r7 i.e. kernel entry values

	ldr	r3, .U8500_L2CC_BASE

	@ if ((readl(U8500_L2CC_BASE + L2X0_CTRL) & 1) == 1)
	ldr	r2, [r3, #L2X0_CTRL]
	tst	r2, #1
	beq	99f

	@ writel(L2X0_WAY_MASK, U8500_L2CC_BASE + L2X0_CLEAN_INV_WAY);
	mov	r2, #L2X0_WAY_MASK
	str	r2, [r3, #L2X0_CLEAN_INV_WAY]

	@ while (readl(U8500_L2CC_BASE + L2X0_CLEAN_INV_WAY)
	@		& L2X0_WAY_MASK)
1:	ldr	r1, [r3, #L2X0_CLEAN_INV_WAY]
	tst	r1, r2
	bne	1b

	@ writel(L2X0_WAY_MASK, U8500_L2CC_BASE + L2X0_DATA_LOCKDOWN_WAY);
	str	r2, [r3, #L2X0_DATA_LOCKDOWN_WAY]
	@ writel(L2X0_WAY_MASK, U8500_L2CC_BASE + L2X0_INST_LOCKDOWN_WAY);
	str	r2, [r3, #L2X0_INST_LOCKDOWN_WAY]
	b	99f

.U8500_L2CC_BASE:
	.word	0xa0412000

99:
