.syntax unified
.arm
.section .text.mb_controls_update,"ax",%progbits
.global mb_controls_update
.type mb_controls_update,%function
mb_controls_update:
    mov r1, r7
    b mb_controls_update_impl
.size mb_controls_update, .-mb_controls_update

// The DDS loader keeps the original FourCC so its surrounding control flow
// remains untouched.  The patched GL format distinguishes the one class whose
// layout is no longer 4x4: opaque DXT1 is uploaded as ASTC 6x6 (0x93b4).
//
// r0 = rglDDS_loader*, r1 = positive mip dimension
// returns ceil(dimension / (format == ASTC_6x6 ? 6 : 4)) in r0
.global mb_texture_block_count
.type mb_texture_block_count,%function
mb_texture_block_count:
    ldr r2, [r0, #0x90]
    movw r3, #0x93b4
    cmp r2, r3
    bne 1f
    add r0, r1, #5
    movw r2, #0xaaab
    movt r2, #0xaaaa
    umull r1, r3, r0, r2
    mov r0, r3, lsr #2
    bx lr
1:
    add r0, r1, #3
    mov r0, r0, lsr #2
    bx lr
.size mb_texture_block_count, .-mb_texture_block_count
