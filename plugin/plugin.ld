/* Minimal link script: H68P payload loads into Secure SRAM window. */
MEMORY
{
  RAM (rwx) : ORIGIN = 0x20040000, LENGTH = 256K
}

SECTIONS
{
  .text : {
    KEEP(*(.text.plugin_entry))
    *(.text*)
    *(.rodata*)
  } > RAM

  .data : { *(.data*) } > RAM
  .bss  : { *(.bss*) *(COMMON) } > RAM

  /* Entry offset 1 = Thumb bit set on origin. */
}
ENTRY(plugin_entry)
