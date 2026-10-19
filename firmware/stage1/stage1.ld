/* noknok stage-1 linker — CH32V003
 *
 * Stage-1 is the real bootloader, relinked from 0x0000 to 0x0400 so the frozen
 * stage-0 can sit below it and replace it in the field.
 *
 *   ORIGIN 0x0400 = the stage-0 reservation (1 KB, frozen — see stage0.ld)
 *   LENGTH 4096   = app_base (0x1400) - stage1_base (0x0400)   (was 3072 / 0x1000)
 *
 * Unlike stage-0, stage-1 DOES link ch32fun.c: it needs the interrupt vector
 * table for the I2C slave handlers.
 *
 * The top 16 bytes of RAM are reserved for the no-init handoff cell at
 * 0x200007F0 (stack ends below it), so the same address is valid in stage-1 and
 * in the application.
 */

ENTRY( InterruptVector )

MEMORY
{
	FLASH (rx) : ORIGIN = 0x00000400, LENGTH = 4096   /* 4 KB stage-1          */
	RAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 2032   /* 2 KB - 16 B (handoff) */
}

/* Image header: base + 0x100, 16 bytes (magic, base, layout, version) */
S1_HDR_OFF  = 0x100;
S1_HDR_SIZE = 16;

/* Sections and ASSERTs: shared with every chip, see stage1_sections.ld */
INCLUDE stage1_sections.ld
