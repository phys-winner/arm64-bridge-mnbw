OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(mb_controls_update)
SECTIONS
{
  . = 0x003CA000;
  .payload : ALIGN(4)
  {
    KEEP(*(.text.mb_controls_update))
    *(.text*)
    *(.rodata*)
  }
  /DISCARD/ :
  {
    *(.interp*)
    *(.comment*)
    *(.note*)
    *(.ARM.attributes*)
    *(.ARM.exidx*)
    *(.ARM.extab*)
    *(.eh_frame*)
  }
}
