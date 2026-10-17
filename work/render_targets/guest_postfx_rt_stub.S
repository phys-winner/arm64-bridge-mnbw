.syntax unified
.arm

.section .hook.guest_postfx_single_channel_float_formats, "ax", %progbits
.global guest_postfx_single_channel_float_formats_hook
.type guest_postfx_single_channel_float_formats_hook, %function
guest_postfx_single_channel_float_formats_hook:
    beq     guest_postfx_rgba16f
.size guest_postfx_single_channel_float_formats_hook, . - guest_postfx_single_channel_float_formats_hook

.section .hook.guest_postfx_float_formats, "ax", %progbits
.global guest_postfx_float_formats_hook
.type guest_postfx_float_formats_hook, %function
guest_postfx_float_formats_hook:
    beq     guest_postfx_rgba16f
.size guest_postfx_float_formats_hook, . - guest_postfx_float_formats_hook

.section .hook.guest_postfx_argb16, "ax", %progbits
.global guest_postfx_argb16_hook
.type guest_postfx_argb16_hook, %function
guest_postfx_argb16_hook:
    b       guest_postfx_rgba16f
.size guest_postfx_argb16_hook, . - guest_postfx_argb16_hook

.section .hook.guest_postfx_render_nodes, "ax", %progbits
.global guest_postfx_render_nodes_hook
.type guest_postfx_render_nodes_hook, %function
guest_postfx_render_nodes_hook:
    bl      guest_postfx_render_nodes
.size guest_postfx_render_nodes_hook, . - guest_postfx_render_nodes_hook

.section .hook.guest_postfx_publish_effect, "ax", %progbits
.global guest_postfx_publish_effect_hook
.type guest_postfx_publish_effect_hook, %function
guest_postfx_publish_effect_hook:
    bl      guest_postfx_publish_effect
.size guest_postfx_publish_effect_hook, . - guest_postfx_publish_effect_hook

/*
 * The mobile backend either omitted storage for D3DFMT_A16B16G16R16,
 * D3DFMT_G16R16F and D3DFMT_A16B16G16R16F, or represented the one-channel
 * D3DFMT_R16F/R32F targets as legacy eight-bit GL_ALPHA.  The original
 * d3dx_create_texture stack frame already has width in r4 and height in r5.
 * Populate the glTexImage2D arguments with a GLES 3.x RGBA16F superset and
 * tail-branch back to the existing guest dispatch.
 */
.section .text.guest_postfx_rgba16f, "ax", %progbits
.global guest_postfx_rgba16f
.type guest_postfx_rgba16f, %function
guest_postfx_rgba16f:
    mov     r0, #0
    movw    r1, #0x140b
    movw    r2, #0x1908
    str     r5, [sp]
    stmib   sp, {r0, r2}
    movw    r2, #0x881a
    str     r1, [sp, #12]
    mov     r1, #0
    str     r0, [sp, #16]
    movw    r0, #0x0de1
    b       guest_tex_image_2d_dispatch
.size guest_postfx_rgba16f, . - guest_postfx_rgba16f

/*
 * The mobile renderer builds a post-FX node vector at render-context offsets
 * 16..24, but its render_all loop never consumes it.  The scene is therefore
 * rendered into the HDR/DOF target and the default framebuffer remains black.
 * Run the bounded vector after render_rc_hlsl has completed the render-target
 * mip chain and viewport restoration.  Running it at the call return samples
 * an incomplete mip-filtered scene texture: permissive Tegra drivers accepted
 * that, while strict GLES implementations return black.  r11 is the renderer
 * and r10 is the current renderer-slot byte offset.  PostFX restoration appends
 * nodes to the source context at slot+0x858 after the render context was copied,
 * so the copied context intentionally has an empty vector.  The final node
 * binds framebuffer zero; intermediate nodes reuse the guest's shared FBO.
 */
.section .text.guest_postfx_render_nodes, "ax", %progbits
.global guest_postfx_render_nodes
.type guest_postfx_render_nodes, %function
guest_postfx_render_nodes:
    push    {r4, r6, r7, lr}

    /*
     * draw_postFX_node looks up techniques through renderer+0xAF60, but the
     * mobile renderer never initializes that field.  Its later attribute path
     * already falls back to the process-wide g_pEffect, which is constructed by
     * initialize_dx_effect_Imp.  Publish the same object in the missing field
     * before consuming the queue.  Derive the ELF load bias from a local label
     * so this remains position-independent after the guest loader relocates the
     * whole image.
     */
.Lpostfx_module_anchor:
    adr     r0, .Lpostfx_module_anchor
    movw    r1, #:lower16:.Lpostfx_module_anchor
    movt    r1, #:upper16:.Lpostfx_module_anchor
    sub     r0, r0, r1
    movw    r1, #:lower16:guest_g_pEffect
    movt    r1, #:upper16:guest_g_pEffect
    ldr     r0, [r0, r1]
    cmp     r0, #0
    movwne  r1, #0xaf60
    strne   r0, [r11, r1]

    add     r4, r11, r10
    ldr     r4, [r4, #0x858]
    ldr     r6, [r4, #16]
    ldr     r7, [r4, #20]
    cmp     r7, r6
    blo     2f
    sub     r0, r7, r6
    cmp     r0, #256
    bhi     2f
1:
    cmp     r6, r7
    bhs     2f
    ldr     r2, [r6], #4
    cmp     r2, #0
    beq     1b
    mov     r0, r11
    mov     r1, r4
    bl      guest_draw_postfx_node
    b       1b
2:
    pop     {r4, r6, r7, lr}
    ldr     r6, [sp, #0x40]
    b       guest_postfx_render_return
.size guest_postfx_render_nodes, . - guest_postfx_render_nodes

/*
 * render_rc_hlsl populates PostFX uniforms while preparing each render
 * context.  Publishing g_pEffect only when the queued nodes are consumed is
 * too late: sampler defaults, exposure timing and DOF parameters have already
 * been skipped.  Hook render_all immediately after it saves this in r11, and
 * reproduce the displaced `mov r6, #0` before returning.  r0 still carries a
 * GOT displacement used by the next original instruction, so preserve it.
 */
.section .text.guest_postfx_publish_effect, "ax", %progbits
.global guest_postfx_publish_effect
.type guest_postfx_publish_effect, %function
guest_postfx_publish_effect:
    push    {r0, r1, lr}
.Lpostfx_entry_anchor:
    adr     r0, .Lpostfx_entry_anchor
    movw    r1, #:lower16:.Lpostfx_entry_anchor
    movt    r1, #:upper16:.Lpostfx_entry_anchor
    sub     r0, r0, r1
    movw    r1, #:lower16:guest_g_pEffect
    movt    r1, #:upper16:guest_g_pEffect
    ldr     r1, [r0, r1]
    cmp     r1, #0
    movwne  r0, #0xaf60
    strne   r1, [r11, r0]
    pop     {r0, r1, lr}
    mov     r6, #0
    bx      lr
.size guest_postfx_publish_effect, . - guest_postfx_publish_effect
