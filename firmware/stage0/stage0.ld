/* noknok stage-0 linker — CH32V003
 *
 * Stage-0 occupies the low 1 KB of flash and is FROZEN once the production
 * batch is programmed. The LENGTH below is therefore a hard contract, not a
 * convenience: stage-1 is linked to start at 0x0400 and stage-0 erases from
 * there upward, so growing this region later would require reflashing every
 * module over SWD. If a build overflows it, do NOT quietly raise it — see
 * docs/stage0-design.md §7.
 *
 * Why 1 KB: the image measures 500 B, and 64-byte erase granularity would in
 * principle have allowed a smaller reservation. But the STAGE-1 BASE MUST BE
 * 1 KB ALIGNED — on this core mtvec only honours the upper address bits, so a
 * vector table at 0x0300 silently becomes 0x0000 and the first interrupt jumps
 * into stage-0's code. Found on the bench 11 Sep 2026: stage-1 at 0x0300 ACKed
 * its I2C address in hardware but every read returned 0xFF because the ISR
 * never ran; moved to 0x0400 it worked first time. Since nothing can live in
 * 0x0300-0x03FF anyway, stage-0 takes the whole 1 KB. 524 B of slack covers
 * pre-freeze fixes and toolchain drift many times over.
 *
 * Empirically: 256 B alignment fails, 1 KB works. 512 B was not tested and is
 * not worth the risk for a frozen component.
 *
 * The top 16 bytes of RAM are reserved for the no-init handoff cell at
 * 0x200007F0 (stack ends below it), so the cell survives stage-0 untouched and
 * stage-1 can still read what the application left there.
 */

ENTRY( InterruptVector )

MEMORY
{
	FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 1024   /* 1 KB stage-0 — FROZEN  */
	RAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 2032   /* 2 KB - 16 B (handoff)  */
}

/* Sections and ASSERTs: shared with every chip, see stage0_sections.ld */
INCLUDE stage0_sections.ld
