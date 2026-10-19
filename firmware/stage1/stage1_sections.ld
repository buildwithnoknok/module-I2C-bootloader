/* noknok stage-1 linker - sections shared by every chip (DEV-124).
 * Included by stage1.ld (CH32V003) and stage1_x035.ld (CH32X035), which set
 * MEMORY plus S1_HDR_OFF / S1_HDR_SIZE (where the image header sits and how
 * big it is: V003 0x100 / 16 B, X035 0x200 / 20 B). Reasons: stage1.ld. */

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
	.stage1_hdr ORIGIN(FLASH) + S1_HDR_OFF : AT(ORIGIN(FLASH) + S1_HDR_OFF)
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
ASSERT( SIZEOF(.init) <= S1_HDR_OFF,
        "stage-1 .init (vector table) has grown past the header offset — it would overlap the image header" )
ASSERT( ADDR(.stage1_hdr) == ORIGIN(FLASH) + S1_HDR_OFF && SIZEOF(.stage1_hdr) == S1_HDR_SIZE,
        "stage-1 image header must be exactly S1_HDR_SIZE bytes at base + S1_HDR_OFF" )
ASSERT( LOADADDR(.stage1_hdr) == ORIGIN(FLASH) + S1_HDR_OFF,
        "stage-1 image header LOAD address must equal its VMA — check the AT()" )
