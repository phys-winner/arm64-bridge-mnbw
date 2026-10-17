OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(guest_settings_activate_hook)

PHDRS
{
    payload PT_LOAD FLAGS(5);
}

/* Original control flow destinations used by guest_settings_entry.S. */
PROVIDE(options_check_event_continue = 0x002AF488);
PROVIDE(options_check_epilogue = 0x002AF4C4);
PROVIDE(options_activate_rejoin = 0x002AEA60);
PROVIDE(controls_activate_rejoin = 0x0029D010);
PROVIDE(controls_check_event_continue = 0x0029E4AC);
PROVIDE(controls_check_epilogue = 0x0029E4E8);
PROVIDE(options_nav_rejoin = 0x002AF41C);
PROVIDE(options_nav_consume = 0x002AF458);
PROVIDE(guest_settings_state_va = 0x0278E400);
PROVIDE(guest_settings_state_end_va = 0x0278E800);

SECTIONS
{
    . = 0x003CB080;
    .payload : ALIGN(4)
    {
        KEEP(*(.text.entry.activate))
        KEEP(*(.text.entry.check))
        KEEP(*(.text.entry.controls_activate))
        KEEP(*(.text.entry.controls_check))
        KEEP(*(.text.entry.options_nav))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
        guest_settings_payload_end = .;
    } :payload

    ASSERT(guest_settings_activate_hook == 0x003CB080,
           "activate hook must be the first payload word")
    ASSERT(guest_settings_payload_end <= 0x003D0A18,
           "payload overlaps the next original PT_LOAD")

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.ARM.attributes)
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.eh_frame*)
    }
}
