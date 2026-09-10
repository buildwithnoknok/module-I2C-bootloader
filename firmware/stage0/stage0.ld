/* noknok stage-0 linker — CH32V003
 *
 * Stage-0 occupies the low 768 bytes of flash and is FROZEN once the production
 * batch is programmed. The LENGTH below is therefore a hard contract, not a
 * convenience: stage-1 is linked to start at 0x0300 and stage-0 erases from
 * there upward, so growing this region later would require reflashing every
 * module over SWD. If a build overflows it, do NOT quietly raise it — see
 * docs/stage0-design.md §7 (measure, round up to a 64-byte boundary, and freeze
 * the new number BEFORE the batch is programmed).
 *
 * Why 768 and not 512: the image measures 500 B. It fits 512, but with only
 * 12 B of slack — and the margin here is not for stage-0 to grow (it never
 * will), it is for fixes found during validation and for toolchain drift. A
 * different GCC at production time emitting a slightly larger image would be a
 * crisis at 12 B of slack and a non-event at 268 B. Stage-1 still gets 3.25 KB
 * against an expected ~2.4 KB, so the margin costs nothing that is needed.
 *
 * The top 16 bytes of RAM are reserved for the no-init handoff cell at
 * 0x200007F0 (stack ends below it), so the cell survives stage-0 untouched and
 * stage-1 can still read what the application left there.
 */

ENTRY( InterruptVector )

MEMORY
{
	FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 768    /* 768 B stage-0 — FROZEN */
	RAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 2032   /* 2 KB - 16 B (handoff)  */
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

/* Stage-0's startup (start.S) does NOT copy .data or zero .bss, because stage-0
 * has neither — all of its state lives in locals. If a future edit introduces a
 * global or a static, that assumption breaks silently and stage-0 would run with
 * uninitialised memory. Fail the build instead. */
ASSERT( SIZEOF(.data) == 0,
        "stage-0 must have no .data — start.S does not copy initialised globals" )
ASSERT( SIZEOF(.bss) == 0,
        "stage-0 must have no .bss — start.S does not zero uninitialised globals" )

/* The reset entry must be the first instruction in flash. */
ASSERT( ADDR(.init) == 0x00000000,
        "stage-0 .init must start at 0x00000000 — the CPU boots there" )
