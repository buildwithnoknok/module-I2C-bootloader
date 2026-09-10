/* SPDX-License-Identifier: MIT
 * Copyright (c) noknok
 *
 * noknok stage-0 — minimal startup (CH32V003, rv32ec)
 *
 * Stage-0 deliberately does NOT link ch32fun.c. That file contributes a full
 * 38-entry interrupt vector table plus a general-purpose reset handler — 248 B
 * measured — none of which stage-0 can use, because it never enables an
 * interrupt. In a component that is frozen at manufacture, every byte is one we
 * can never reclaim, and every symbol we don't link is one that can't surprise
 * us later.
 *
 * So the entire startup is: point gp and sp somewhere valid, call main().
 *
 * There is no .data copy and no .bss zeroing here, because stage-0 has neither
 * (all of its state is locals in registers/stack). stage0.ld ASSERTs that this
 * stays true — if a future edit introduces a global, the build fails loudly
 * rather than silently running with uninitialised memory.
 *
 * The reset entry must be the very first thing in flash: the CH32V003 begins
 * executing at 0x0000_0000 on every reset.
 */

	.section .init, "ax"
	.global InterruptVector
	.align 2
InterruptVector:
	j       handle_reset          /* the only vector stage-0 needs */

	.section .text.handle_reset, "ax"
	.global handle_reset
	.align 2
handle_reset:
.option push
.option norelax
	la      gp, __global_pointer$
.option pop
	la      sp, _eusrstack        /* = 0x200007F0, below the handoff cell */
	jal     ra, main
1:	j       1b                    /* main() never returns */
