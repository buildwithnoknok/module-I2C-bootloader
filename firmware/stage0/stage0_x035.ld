/* noknok stage-0 linker — CH32X035 (noknok PicoHub power manager), DEV-124
 *
 * Same frozen contract as the CH32V003 (stage0.ld, read it for the reasons):
 * stage-0 = the low 1 KB, stage-1 starts at 0x0400 (1 KB aligned for mtvec).
 * Only RAM differs: 20 KB, top 16 B reserved for the no-init cells at
 * 0x20004FF0 (handoff, attempts), so the stack ends below them.
 */

ENTRY( InterruptVector )

MEMORY
{
	FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 1024   /* 1 KB stage-0 — FROZEN  */
	RAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 20464  /* 20 KB - 16 B (handoff @0x20004FF0) */
}

/* Sections and ASSERTs: shared with every chip, see stage0_sections.ld */
INCLUDE stage0_sections.ld
