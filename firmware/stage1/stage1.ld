/* noknok stage-1 linker — CH32V003
 *
 * Stage-1 is the real bootloader, relinked from 0x0000 to 0x0400 so the frozen
 * stage-0 can sit below it and replace it in the field.
 *
 *   ORIGIN 0x0400 = the stage-0 reservation (1 KB, frozen — see stage0.ld)
 *   LENGTH 3072   = app_base (0x1000) - stage1_base (0x0400)
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
	FLASH (rx) : ORIGIN = 0x00000400, LENGTH = 3072   /* 3 KB stage-1          */
	RAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 2032   /* 2 KB - 16 B (handoff) */
}

SECTIONS
{
	.init :
	{
		_sinit = .;
		. = ALIGN(4);
		KEEP(*(SORT_NONE(.init)))
		*(.text.vector_handler)
		. = ALIGN(4);
		_einit = .;
	} >FLASH AT>FLASH

	/* Image header at a FIXED offset (base + 0x100), just past the vector
	 * table. VERIFY_STAGE1 reads a staged image here to confirm it really is a
	 * stage-1 built for this layout before it arms stage-0 (hardening E). The
	 * offset is a contract with stage-1's do_verify_stage1() — do not move it. */
	.stage1_hdr ORIGIN(FLASH) + 0x100 : AT(ORIGIN(FLASH) + 0x100)
	{
		KEEP(*(.stage1_hdr))
	} >FLASH
	/* The explicit AT() matters: "AT>FLASH" would continue the load-address
	 * counter from the end of .init (0x4F8), putting the header at file offset
	 * 0xF8 while VERIFY_STAGE1 reads 0x100. Caught on the first build. */

	.text :
	{
		. = ALIGN(4);
		*(.text)
		*(.text.*)
		*(.rodata)
		*(.rodata*)
		*(.gnu.linkonce.t.*)
		. = ALIGN(4);
	} >FLASH AT>FLASH

	.fini :
	{
		KEEP(*(SORT_NONE(.fini)))
		. = ALIGN(4);
	} >FLASH AT>FLASH

	PROVIDE( _etext = . );

	.preinit_array :
	{
		PROVIDE_HIDDEN (__preinit_array_start = .);
		KEEP (*(.preinit_array))
		PROVIDE_HIDDEN (__preinit_array_end = .);
	} >FLASH AT>FLASH

	.init_array :
	{
		PROVIDE_HIDDEN (__init_array_start = .);
		KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
		KEEP (*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .ctors))
		PROVIDE_HIDDEN (__init_array_end = .);
	} >FLASH AT>FLASH

	.fini_array :
	{
		PROVIDE_HIDDEN (__fini_array_start = .);
		KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
		KEEP (*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .dtors))
		PROVIDE_HIDDEN (__fini_array_end = .);
	} >FLASH AT>FLASH

	.dalign :
	{
		. = ALIGN(4);
		PROVIDE(_data_vma = .);
	} >RAM AT>FLASH

	.dlalign :
	{
		. = ALIGN(4);
		PROVIDE(_data_lma = .);
	} >FLASH AT>FLASH

	.data :
	{
		. = ALIGN(4);
		*(.data.vector_handler)
		*(.gnu.linkonce.r.*)
		*(.data .data.*)
		*(.gnu.linkonce.d.*)
		. = ALIGN(8);
		PROVIDE( __global_pointer$ = . + 0x800 );
		*(.sdata .sdata.*)
		*(.sdata2*)
		*(.gnu.linkonce.s.*)
		. = ALIGN(8);
		*(.srodata.cst16)
		*(.srodata.cst8)
		*(.srodata.cst4)
		*(.srodata.cst2)
		*(.srodata .srodata.*)
		. = ALIGN(4);
		PROVIDE( _edata = .);
	} >RAM AT>FLASH

	.bss :
	{
		. = ALIGN(4);
		PROVIDE( _sbss = .);
		*(.sbss*)
		*(.gnu.linkonce.sb.*)
		*(.bss*)
		*(.gnu.linkonce.b.*)
		*(COMMON*)
		. = ALIGN(4);
		PROVIDE( _ebss = .);
	} >RAM

	PROVIDE( _end = _ebss);
	PROVIDE( end = . );

	PROVIDE( _eusrstack = ORIGIN(RAM) + LENGTH(RAM));   /* = 0x200007F0 */
}

/* Stage-1 must start exactly at the stage-0 boundary. If these ever disagree,
 * stage-0 would jump into the middle of nothing. */
ASSERT( ADDR(.init) == 0x00000400,
        "stage-1 must start at 0x0400 — the frozen stage-0 boundary" )

/* And that boundary MUST be 1 KB aligned. The vector table lives at the start
 * of stage-1 and mtvec only honours the upper address bits on this core — a
 * table at 0x0300 silently became 0x0000 on the bench (11 Sep 2026) and the
 * first I2C interrupt jumped into stage-0's code. Kept as a separate check so
 * it still guards if the base is ever moved (e.g. to 0x0800 for a larger
 * stage-0). */
ASSERT( (ADDR(.init) & 0x3FF) == 0,
        "stage-1 base must be 1 KB aligned — mtvec ignores the low bits" )

/* The vector table must not grow into the header slot. */
ASSERT( SIZEOF(.init) <= 0x100,
        "stage-1 .init (vector table) has grown past 0x100 — it would overlap the image header" )
ASSERT( ADDR(.stage1_hdr) == ORIGIN(FLASH) + 0x100 && SIZEOF(.stage1_hdr) == 16,
        "stage-1 image header must be exactly 16 bytes at base + 0x100" )
ASSERT( LOADADDR(.stage1_hdr) == ORIGIN(FLASH) + 0x100,
        "stage-1 image header LOAD address must equal its VMA — check the AT()" )
