OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(guest_postfx_rgba16f)

PHDRS
{
    hooks PT_LOAD FLAGS(5);
    payload PT_LOAD FLAGS(5);
}

PROVIDE(guest_tex_image_2d_dispatch = 0x00212B60);
PROVIDE(guest_draw_postfx_node = 0x001EAEF8);
PROVIDE(guest_postfx_render_return = 0x001ECE40);
PROVIDE(guest_g_pEffect = 0x0044F2C4);

SECTIONS
{
    . = 0x001EC6A8;
    .hook_publish_effect : ALIGN(4)
    {
        KEEP(*(.hook.guest_postfx_publish_effect))
    } :hooks

    . = 0x001ECE3C;
    .hook_render_nodes : ALIGN(4)
    {
        KEEP(*(.hook.guest_postfx_render_nodes))
    } :hooks

    . = 0x00212B2C;
    .hook_single_channel_float_formats : ALIGN(4)
    {
        KEEP(*(.hook.guest_postfx_single_channel_float_formats))
    } :hooks

    . = 0x00212BC8;
    .hook_float_formats : ALIGN(4)
    {
        KEEP(*(.hook.guest_postfx_float_formats))
    } :hooks

    . = 0x00212C84;
    .hook_argb16 : ALIGN(4)
    {
        KEEP(*(.hook.guest_postfx_argb16))
    } :hooks

    . = 0x003CE000;
    .payload : ALIGN(4)
    {
        KEEP(*(.text.guest_postfx_rgba16f))
        KEEP(*(.text.guest_postfx_render_nodes))
        KEEP(*(.text.guest_postfx_publish_effect))
        . = ALIGN(4);
        guest_postfx_rt_payload_end = .;
    } :payload

    ASSERT(guest_postfx_rgba16f == 0x003CE000,
           "PostFX render-target stub moved")
    ASSERT(guest_postfx_float_formats_hook == 0x00212BC8,
           "PostFX float-format hook moved")
    ASSERT(guest_postfx_single_channel_float_formats_hook == 0x00212B2C,
           "PostFX single-channel float-format hook moved")
    ASSERT(guest_postfx_argb16_hook == 0x00212C84,
           "PostFX A16B16G16R16 hook moved")
    ASSERT(guest_postfx_render_nodes_hook == 0x001ECE3C,
           "PostFX render-node hook moved")
    ASSERT(guest_postfx_publish_effect_hook == 0x001EC6A8,
           "PostFX early effect hook moved")
    ASSERT(guest_postfx_rt_payload_end <= 0x003D0A18,
           "PostFX render-target stub overlaps the next original PT_LOAD")

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
