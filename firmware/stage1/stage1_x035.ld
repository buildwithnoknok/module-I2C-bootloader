/* noknok stage-1 linker — CH32X035 (noknok PicoHub power manager), DEV-124
 *
 * Same rules as the CH32V003 (stage1.ld, read it for the reasons): stage-1 at
 * 0x0400, 4 KB, 1 KB aligned for mtvec. Differences: RAM is 20 KB (no-init
 * cells at 0x20004FF0), and the X035 vector table is bigger than 0x100 bytes,
 * so the image header sits at base + 0x200 and carries a 5th word, the target
 * id "X035" (20 bytes). VERIFY_STAGE1 reads it at the same offset.
 */

ENTRY( InterruptVector )

MEMORY
{
	FLASH (rx) : ORIGIN = 0x00000400, LENGTH = 4096   /* 4 KB stage-1          */
	RAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 20464  /* 20 KB - 16 B (handoff @0x20004FF0) */
}

/* Image header: base + 0x200, 20 bytes (magic, base, layout, version, target) */
S1_HDR_OFF  = 0x200;
S1_HDR_SIZE = 20;

/* Sections and ASSERTs: shared with every chip, see stage1_sections.ld */
INCLUDE stage1_sections.ld
