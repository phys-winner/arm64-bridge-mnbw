.syntax unified
.arm

/*
 * Options_window::activate hook at 0x002AEA5C, after the original RB-tree
 * navigation finalizer.  Running the builder before that loop mutates the
 * widget tree while the finalizer is traversing it and can leave the guest in
 * an unbounded _Rb_tree_increment loop.  The replaced instruction is
 * `ldr r0, [pc, #0x88]`, whose literal value is replayed below.  The source
 * hook is an unconditional B, so this veneer must rejoin explicitly instead
 * of trusting the caller's stale LR.  Six saved registers keep SP aligned to
 * 8 bytes for the AAPCS call into C.
 */
.section .text.entry.activate,"ax",%progbits
.align 2
.global guest_settings_activate_hook
.type guest_settings_activate_hook,%function
guest_settings_activate_hook:
    push {r0-r3, r12, lr}
    mov r0, r5
    adr r1, guest_settings_activate_hook
    bl guest_settings_activate_impl
    pop {r0-r3, r12, lr}
    ldr r0, .Loptions_activate_displaced_literal
    b options_activate_rejoin
.size guest_settings_activate_hook, .-guest_settings_activate_hook

/*
 * Options_window::check hook at 0x002AF484.
 * It replaces `bne 0x002AF4C4` and therefore receives the flags from the
 * original `cmp r0, #1`.  Non-events still run the cheap payload tick before
 * taking the original epilogue.  Event 14 is intercepted; every other event
 * rejoins the pristine handler at 0x002AF488.
 */
.section .text.entry.check,"ax",%progbits
.align 2
.global guest_settings_check_dispatch
.type guest_settings_check_dispatch,%function
guest_settings_check_dispatch:
    bne .Lguest_settings_idle
    push {r0-r3, r12, lr}
    mov r0, r4
    adr r1, guest_settings_activate_hook
    bl guest_settings_handle_event
    cmp r0, #0
    pop {r0-r3, r12, lr}
    bne options_check_epilogue
    b options_check_event_continue

.Lguest_settings_idle:
    push {r0-r3, r12, lr}
    mov r0, r4
    adr r1, guest_settings_activate_hook
    bl guest_settings_tick
    pop {r0-r3, r12, lr}
    b options_check_epilogue
.size guest_settings_check_dispatch, .-guest_settings_check_dispatch

/*
 * Controls_window::activate post-add hook at 0x0029D00C.  Both pristine tab
 * items (13 and 15) already exist here, so appending item 14 yields the same
 * [13, 15, 14] order as Options_window.  Replay the displaced
 * `ldr r0, [r11, #0x1b8]` before the explicit rejoin.
 */
.section .text.entry.controls_activate,"ax",%progbits
.align 2
.global guest_settings_controls_activate_hook
.type guest_settings_controls_activate_hook,%function
guest_settings_controls_activate_hook:
    push {r0-r3, r12, lr}
    mov r0, r11
    adr r1, guest_settings_activate_hook
    bl guest_settings_controls_activate_impl
    pop {r0-r3, r12, lr}
    ldr r0, [r11, #0x1b8]
    b controls_activate_rejoin
.size guest_settings_controls_activate_hook, .-guest_settings_controls_activate_hook

/*
 * Controls_window::check hook at 0x0029E4A8.  It replaces the conditional
 * pop after `cmp r0, #1`.  The C dispatcher receives the tab event result in
 * r2 and chooses either the pristine event path or the common epilogue.
 */
.section .text.entry.controls_check,"ax",%progbits
.align 2
.global guest_settings_controls_check_dispatch
.type guest_settings_controls_check_dispatch,%function
guest_settings_controls_check_dispatch:
    push {r0-r3, r12, lr}
    mov r2, r0
    mov r0, r8
    mov r1, r6
    adr r3, guest_settings_activate_hook
    bl guest_settings_controls_check_impl
    cmp r0, #0
    pop {r0-r3, r12, lr}
    beq controls_check_epilogue
    b controls_check_event_continue
.size guest_settings_controls_check_dispatch, .-guest_settings_controls_check_dispatch

/*
 * Options_window::check shoulder-navigation hook at 0x002AF418.  Result 0
 * replays `sub r0, r8, #103`; result 1 consumes a right-edge navigation;
 * result 2 rewrites the event to the pristine Controls transition.
 */
.section .text.entry.options_nav,"ax",%progbits
.align 2
.global guest_settings_options_nav_dispatch
.type guest_settings_options_nav_dispatch,%function
guest_settings_options_nav_dispatch:
    push {r0-r3, r12, lr}
    mov r0, r4
    mov r1, r8
    adr r2, guest_settings_activate_hook
    bl guest_settings_options_nav_impl
    cmp r0, #2
    beq .Lguest_settings_nav_controls
    cmp r0, #1
    beq .Lguest_settings_nav_consume
    pop {r0-r3, r12, lr}
    sub r0, r8, #103
    b options_nav_rejoin

.Lguest_settings_nav_controls:
    pop {r0-r3, r12, lr}
    mov r8, #103
    sub r0, r8, #103
    b options_nav_rejoin

.Lguest_settings_nav_consume:
    pop {r0-r3, r12, lr}
    b options_nav_consume
.size guest_settings_options_nav_dispatch, .-guest_settings_options_nav_dispatch

/* Literal loaded by the displaced instruction at 0x002AEA5C. */
.align 2
.Loptions_activate_displaced_literal:
    .word 0x001286BC

.section .note.GNU-stack,"",%progbits
