#!/usr/bin/env python3
"""Fail-closed static proof for the isolated ARM32 settings payload."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_LITTLE_ENDIAN, Cs
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_ARM, Uc, UcError
from unicorn.arm_const import (
    UC_ARM_REG_C1_C0_2,
    UC_ARM_REG_FPEXC,
    UC_ARM_REG_LR,
    UC_ARM_REG_PC,
    UC_ARM_REG_R0,
    UC_ARM_REG_R1,
    UC_ARM_REG_R11,
    UC_ARM_REG_R12,
    UC_ARM_REG_R2,
    UC_ARM_REG_R3,
    UC_ARM_REG_R4,
    UC_ARM_REG_R5,
    UC_ARM_REG_R6,
    UC_ARM_REG_R8,
    UC_ARM_REG_SP,
)


PAYLOAD_VA = 0x003CB080
NEXT_LOAD_VA = 0x003D0A18
DIRECT_LOOK_STUB_VA = 0x003CB000
DIRECT_LOOK_STUB_END = 0x003CB078
STATE_VA = 0x0278E400
STATE_END_VA = 0x0278E800
PENDING_CUSTOM_VA = STATE_VA + 0x2B8
PENDING_CUSTOM_MAGIC = 0x444E4550
ACTIVATE_HOOK_VA = 0x002AEA5C
ACTIVATE_REJOIN_VA = 0x002AEA60
CHECK_HOOK_VA = 0x002AF484
CHECK_EVENT_CONTINUE_VA = 0x002AF488
CHECK_EPILOGUE_VA = 0x002AF4C4
CONTROLS_ACTIVATE_HOOK_VA = 0x0029D00C
CONTROLS_ACTIVATE_REJOIN_VA = 0x0029D010
CONTROLS_CHECK_HOOK_VA = 0x0029E4A8
CONTROLS_CHECK_EVENT_CONTINUE_VA = 0x0029E4AC
CONTROLS_CHECK_EPILOGUE_VA = 0x0029E4E8
OPTIONS_NAV_HOOK_VA = 0x002AF418
OPTIONS_NAV_REJOIN_VA = 0x002AF41C
OPTIONS_NAV_CONSUME_VA = 0x002AF458
ACTIVATE_PREIMAGE = bytes.fromhex("88 00 9f e5")
CHECK_PREIMAGE = bytes.fromhex("0e 00 00 1a")
CONTROLS_ACTIVATE_PREIMAGE = bytes.fromhex("b8 01 9b e5")
CONTROLS_CHECK_PREIMAGE = bytes.fromhex("f0 8b bd 18")
OPTIONS_NAV_PREIMAGE = bytes.fromhex("67 00 48 e2")
LANGUAGE_LITERAL_VA = 0x002564CC
LANGUAGE_GOT_BASE = 0x003D7128
EXPECTED_LANGUAGE_WORD = 0xFFFCC508

STRING_CTOR_VA = 0x0020DDF0
STRING_DTOR_VA = 0x0020DF1C
CREATE_TEXT_VA = 0x00131D24
CREATE_CHECK_BOX_VA = 0x001331F8
WIDGET_HIDE_VA = 0x00135394
WIDGET_UNHIDE_VA = 0x001353A4
WIDGET_DISABLE_VA = 0x001353B4
WIDGET_UNDISABLE_VA = 0x001353C4
TAB_ADD_ITEM_VA = 0x0021E538
CREATE_VERTICAL_SCROLLABLE_VA = 0x002A83C0
CREATE_SLIDER_HOR_VA = 0x002A91B4
CREATE_COMBO_LABEL_VA = 0x001320F0
COMBO_ADD_ITEM_VA = 0x0013ACF8
COMBO_SET_VALUE_I_VA = 0x0013AB68
UPDATE_SCROLLABLE_BOARD_VA = 0x002A9690
CHECK_BOX_SET_VALUE_I_VA = 0x00138370
WIDGET_SET_VALUE_F_VA = 0x00133DC0
SET_ACTIVE_WIDGET_HIGHLIGHT_VA = 0x00214E94
RGL_WINDOW_MANAGER_VA = 0x003E6188
RGL_WINDOW_MANAGER_ACTIVE_OFFSET = 0x0AD4

FORBIDDEN_LAYOUT_VAS = {
    0x00352B90: "operator new",
    0x00133480: "manual rglContainer_widget constructor",
    0x002150BC: "manual set_position",
    0x00215180: "manual set_size",
    0x00135608: "manual add_child",
    0x0029EF00: "second create_tab_panel",
}

CHECKBOX_INTERNAL_NAMES = (
    "mb_gyro_enabled_check_box",
    "mb_gyro_invert_y_check_box",
    "mb_menu_swipe_check_box",
    "mb_gyro_ranged_check_box",
    "mb_left_handed_check_box",
    "mb_hdr_check_box",
    "mb_fxaa_check_box",
    "mb_shadow_mode_check_box",
    "mb_autoexp_check_box",
    "mb_haptic_check_box",
    "mb_menu_aspect_check_box",
)
TEXT_INTERNAL_NAMES = (
    "mb_move_sensitivity_text",
    "mb_gyro_sensitivity_text",
    "mb_gyro_deadzone_text",
    "mb_button_scale_text",
    "mb_button_opacity_text",
    "mb_fps_limit_text",
    "mb_hdr_brightness_text",
    "mb_map_pan_sensitivity_text",
    "mb_report_text_scale_text",
)
SLIDER_INTERNAL_NAMES = (
    "mb_move_sensitivity_slider",
    "mb_gyro_sensitivity_slider",
    "mb_gyro_deadzone_slider",
    "mb_button_scale_slider",
    "mb_button_opacity_slider",
    "mb_hdr_brightness_slider",
    "mb_map_pan_sensitivity_slider",
    "mb_report_text_scale_slider",
)
COMBO_INTERNAL_NAMES = (
    "mb_fps_limit_combo",
)
# Visible labels in payload creation order (top row first).
VISIBLE_EN = (
    "Additional",
    "Swipe vertically to navigate menus",
    "Apply gyroscope to ranged weapons only",
    "Left-handed control layout",
    "Button opacity",
    "Button size",
    "Movement sensitivity",
    "Frame rate limit",
    "Gyroscope aiming",
    "Invert gyroscope vertical axis",
    "Gyroscope sensitivity",
    "Gyroscope deadzone",
    "HDR",
    "Edge smoothing (FXAA)",
    "HDR automatic exposure",
    "HDR brightness",
    "Foliage shadows",
    "Touch vibration",
    "Map camera speed",
    "Keep interface proportions (16:9 / 4:3)",
    "Combat report text scale",
)
VISIBLE_RU = (
    "Дополнительные",
    "Листать меню вертикальными свайпами",
    "Гироскоп только для дальнобойного оружия",
    "Управление для левшей",
    "Прозрачность кнопок",
    "Размер кнопок",
    "Чувствительность движения",
    "Лимит кадров",
    "Прицеливание гироскопом",
    "Инвертировать вертикальную ось гироскопа",
    "Чувствительность гироскопа",
    "Мёртвая зона гироскопа",
    "HDR",
    "Сглаживание краёв (FXAA)",
    "Автоэкспозиция HDR",
    "Яркость HDR",
    "Тени листвы",
    "Вибрация сенсорного управления",
    "Скорость камеры карты",
    "Сохранять пропорции интерфейса (16:9 / 4:3)",
    "Масштаб боевых сообщений",
)

# Creation-order layout for the graphics group followed by controls/gyro.
CHECKBOX_LAYOUT = (
    (CHECKBOX_INTERNAL_NAMES[5], 0.057, 1.010, 0.025),
    (CHECKBOX_INTERNAL_NAMES[8], 0.057, 0.970, 0.025),
    (CHECKBOX_INTERNAL_NAMES[6], 0.057, 0.890, 0.025),
    (CHECKBOX_INTERNAL_NAMES[7], 0.057, 0.850, 0.025),
    (CHECKBOX_INTERNAL_NAMES[2], 0.057, 0.810, 0.025),
    (CHECKBOX_INTERNAL_NAMES[9], 0.057, 0.770, 0.025),
    (CHECKBOX_INTERNAL_NAMES[4], 0.057, 0.730, 0.025),
    (CHECKBOX_INTERNAL_NAMES[10], 0.057, 0.490, 0.025),
    (CHECKBOX_INTERNAL_NAMES[0], 0.057, 0.410, 0.025),
    (CHECKBOX_INTERNAL_NAMES[3], 0.057, 0.370, 0.025),
    (CHECKBOX_INTERNAL_NAMES[1], 0.057, 0.330, 0.025),
)
TEXT_LAYOUT = (
    (TEXT_INTERNAL_NAMES[6], 0.050, 0.930, 0.025),
    (TEXT_INTERNAL_NAMES[4], 0.050, 0.690, 0.025),
    (TEXT_INTERNAL_NAMES[3], 0.050, 0.650, 0.025),
    (TEXT_INTERNAL_NAMES[0], 0.050, 0.610, 0.025),
    (TEXT_INTERNAL_NAMES[7], 0.050, 0.570, 0.025),
    (TEXT_INTERNAL_NAMES[5], 0.050, 0.530, 0.025),
    (TEXT_INTERNAL_NAMES[8], 0.050, 0.450, 0.025),
    (TEXT_INTERNAL_NAMES[1], 0.050, 0.290, 0.025),
    (TEXT_INTERNAL_NAMES[2], 0.050, 0.250, 0.025),
)
SLIDER_LAYOUT = (
    (SLIDER_INTERNAL_NAMES[5], 0.433, 0.928, 0.020),
    (SLIDER_INTERNAL_NAMES[4], 0.433, 0.688, 0.020),
    (SLIDER_INTERNAL_NAMES[3], 0.433, 0.648, 0.020),
    (SLIDER_INTERNAL_NAMES[0], 0.433, 0.608, 0.020),
    (SLIDER_INTERNAL_NAMES[6], 0.433, 0.568, 0.020),
    (SLIDER_INTERNAL_NAMES[7], 0.433, 0.448, 0.020),
    (SLIDER_INTERNAL_NAMES[1], 0.433, 0.288, 0.020),
    (SLIDER_INTERNAL_NAMES[2], 0.433, 0.248, 0.020),
)
COMBO_LAYOUT = (
    (COMBO_INTERNAL_NAMES[0], 0.402, 0.528, 0.020),
)

# Visible-widget index groups mirrored by the layout builder:
CHECKBOX_VISIBLE = (12, 14, 13, 16, 1, 17, 3, 19, 8, 2, 9)
TEXTSLIDER_VISIBLE = (15, 4, 5, 6, 18, 7, 20, 10, 11)
TEXTSLIDER_LABELS = TEXTSLIDER_VISIBLE

HOOK_SPECS = (
    ("activate", ACTIVATE_HOOK_VA, ACTIVATE_PREIMAGE,
     "guest_settings_activate_hook"),
    ("check", CHECK_HOOK_VA, CHECK_PREIMAGE,
     "guest_settings_check_dispatch"),
    ("controls_activate", CONTROLS_ACTIVATE_HOOK_VA,
     CONTROLS_ACTIVATE_PREIMAGE, "guest_settings_controls_activate_hook"),
    ("controls_check", CONTROLS_CHECK_HOOK_VA,
     CONTROLS_CHECK_PREIMAGE, "guest_settings_controls_check_dispatch"),
    ("options_nav", OPTIONS_NAV_HOOK_VA, OPTIONS_NAV_PREIMAGE,
     "guest_settings_options_nav_dispatch"),
)

EXPECTED_PAYLOAD_STRINGS = (
    "mb_guest_settings",
    *CHECKBOX_INTERNAL_NAMES,
    *TEXT_INTERNAL_NAMES,
    *SLIDER_INTERNAL_NAMES,
    *COMBO_INTERNAL_NAMES,
    *VISIBLE_EN,
    *VISIBLE_RU,
)

KNOWN_GUEST_HASHES = {
    "71dc755de096fbb29c35e6a4051c6de70dd7d2385b8715809fb51842f0e4e358": "pristine",
    "bc18be84661c960850dbe7562d9ee2f6f662984e3916cb061da32a4c8cd1499e": "IDA snapshot",
    "a12f803f20ee00ecf802ff6b01c4efaa33ef37b9b07e9e3485b19fbe246d3eef": "pre-settings staged guest",
}

EXPECTED_DYNSYMS = {
    "_Z18widget_create_textP19rglContainer_widgetRK9rglStringS3_RKfS5_S5_": 0x00131D24,
    "_Z23widget_create_check_boxP19rglContainer_widgetRK9rglStringS3_RKfS5_S5_": 0x001331F8,
    "_ZN14rglWidget_base11set_value_iEi": 0x00133DB0,
    "_ZN14rglWidget_base11set_value_fEf": 0x00133DC0,
    "_ZN14rglWidget_base4hideEv": 0x00135394,
    "_ZN14rglWidget_base6unhideEv": 0x001353A4,
    "_ZN14rglWidget_base7disableEv": 0x001353B4,
    "_ZN14rglWidget_base9undisableEv": 0x001353C4,
    "_ZN19rglCheck_box_widget11set_value_iEi": 0x00138370,
    "_ZN9rglStringC1EPKc": 0x0020DDF0,
    "_ZN9rglStringD1Ev": 0x0020DF1C,
    "_ZN17rglWindow_manager30set_active_widget_to_highlightEP14rglWidget_base": 0x00214E94,
    "_ZN13rglTab_widget8add_itemERK9rglStringi": 0x0021E538,
    "_Z16initialize_stuffPv": 0x002561D4,
    "_ZN15Controls_window8activateEv": 0x0029C8B0,
    "_ZN15Controls_window5checkEf": 0x0029E15C,
    "_Z33widget_create_vertical_scrollableP19rglContainer_widgetRK9rglStringRKfS5_S5_S5_": 0x002A83C0,
    "_Z24widget_create_slider_horP19rglContainer_widgetRK9rglStringRKfS5_S5_": 0x002A91B4,
    "_Z25widget_create_combo_labelP19rglContainer_widgetRK9rglStringRKfS5_S5_": 0x001320F0,
    "_ZN21rglCombo_label_widget8add_itemERK9rglString": 0x0013ACF8,
    "_ZN21rglCombo_label_widget11set_value_iEi": 0x0013AB68,
    "_ZN29rglVertical_scrollable_widget45update_board_size_and_position_using_childrenEv": 0x002A9690,
    "_ZN14Options_window8activateEv": 0x002A9A18,
    "_ZN14Options_window5checkEf": 0x002AEAF0,
    "rgl_window_manager": 0x003E6188,
}

EXPECTED_INSTRUCTIONS = {
    0x00131D2C: ("mov", "r8, r1"),
    0x00131D38: ("mov", "fp, r3"),
    0x00131D40: ("mov", "sl, r2"),
    0x00131E08: ("ldr", "ip, [sp, #0x298]"),
    0x00131E28: ("ldr", "r6, [sp, #0x29c]"),
    0x00133200: ("mov", "r8, r0"),
    0x0013320C: ("mov", "r5, r3"),
    0x00133214: ("mov", "sb, r2"),
    0x00133218: ("mov", "sl, r1"),
    0x00133248: ("ldr", "r0, [sp, #0x288]"),
    0x00133250: ("ldr", "r6, [sp, #0x28c]"),
    0x00133DB0: ("str", "r1, [r0, #0x2b0]"),
    0x00133DC0: ("str", "r1, [r0, #0x2bc]"),
    0x00135394: ("ldr", "r1, [r0, #0x1bc]"),
    0x00135398: ("orr", "r1, r1, #1"),
    0x001353A4: ("ldr", "r1, [r0, #0x1bc]"),
    0x001353B4: ("ldr", "r1, [r0, #0x1bc]"),
    0x001353B8: ("orr", "r1, r1, #2"),
    0x00138398: ("str", "r0, [r4, #0x2b0]"),
    0x0020DDF8: ("mov", "r0, #0x80"),
    0x0020DDFC: ("add", "r7, r4, #0x10"),
    0x00214E9C: ("mov", "r8, r0"),
    0x00214EA4: ("mov", "r4, r1"),
    0x00214EBC: ("ldr", "r0, [r8, #0xad4]"),
    0x00214EC0: ("cmp", "r0, r4"),
    0x00214FAC: ("cmp", "r4, #0"),
    0x00215060: ("str", "r4, [r8, #0xad4]"),
    0x002179FC: ("ldr", "r0, [r5, #0x380]"),
    0x00217A64: ("ldr", "r0, [r0, #0x2d0]"),
    0x0021E548: ("str", "r2, [r4, #0x380]"),
    0x0029D000: ("blx", "r7"),
    CONTROLS_ACTIVATE_HOOK_VA: ("ldr", "r0, [fp, #0x1b8]"),
    CONTROLS_ACTIVATE_REJOIN_VA: ("ldr", "r1, [fp, #0x1bc]"),
    0x0029E43C: ("cmp", "r6, #0x66"),
    0x0029E440: ("cmpne", "r6, #0x69"),
    0x0029E4A0: ("blx", "r2"),
    0x0029E4A4: ("cmp", "r0, #1"),
    CONTROLS_CHECK_HOOK_VA: ("popne", "{r4, r5, r6, r7, r8, sb, fp, pc}"),
    CONTROLS_CHECK_EVENT_CONTINUE_VA: ("ldr", "r0, [pc, #0x70]"),
    0x002A83D0: ("mov", "sl, r1"),
    0x002A83D8: ("mov", "sb, r3"),
    0x002A83E0: ("mov", "r8, r2"),
    0x002A845C: ("ldr", "r5, [sp, #0x7b4]"),
    0x002A8460: ("ldr", "r7, [sp, #0x7b0]"),
    0x002A8754: ("ldr", "r0, [r4, #0x748]"),
    0x002A878C: ("ldr", "r0, [sp, #8]"),
    0x002A8798: ("mov", "r1, r4"),
    0x002A879C: ("blx", "r2"),
    0x002A91C0: ("mov", "sl, r1"),
    0x002A91C8: ("mov", "r8, r3"),
    0x002A91D4: ("mov", "sb, r2"),
    0x002A91D8: ("mov", "fp, r0"),
    0x002A92EC: ("ldr", "r6, [sp, #0x358]"),
    0x002A969C: ("mov", "r4, r0"),
    0x002A96A8: ("ldr", "r0, [r4, #0x748]"),
    0x002A9A28: ("mov", "r5, r0"),
    0x002A9B38: ("mov", "r0, #0x370"),
    0x002A9C0C: ("str", "r5, [sb, #0x1b8]"),
    0x002A9D74: ("str", "r8, [sb, #0x1bc]"),
    ACTIVATE_HOOK_VA: ("ldr", "r0, [pc, #0x88]"),
    0x002AEAFC: ("mov", "r4, r0"),
    OPTIONS_NAV_HOOK_VA: ("sub", "r0, r8, #0x67"),
    OPTIONS_NAV_REJOIN_VA: ("cmp", "r0, #1"),
    0x002AF420: ("bhi", "#0x2af458"),
    0x002AF470: ("ldr", "r1, [r0]"),
    0x002AF480: ("cmp", "r0, #1"),
    CHECK_HOOK_VA: ("bne", "#0x2af4c4"),
}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def arm_branch(source: int, target: int, *, link: bool) -> bytes:
    delta = target - (source + 8)
    if source & 3 or target & 3 or delta & 3:
        raise AssertionError("unaligned ARM branch")
    if delta < -(1 << 25) or delta > (1 << 25) - 4:
        raise AssertionError("ARM branch target is out of range")
    opcode = 0xEB000000 if link else 0xEA000000
    return struct.pack("<I", opcode | ((delta // 4) & 0x00FFFFFF))


def one_instruction(data: bytes, address: int) -> tuple[str, str]:
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN)
    decoded = list(md.disasm(data, address, count=1))
    if len(decoded) != 1 or decoded[0].size != 4:
        raise AssertionError(f"cannot decode ARM word at {address:#x}")
    return decoded[0].mnemonic, decoded[0].op_str


def verify_activate_rejoin(payload: bytes, payload_symbols: dict[str, tuple[int, int, object]]) -> None:
    """Prove the B-entered veneer cannot return through a stale LR."""
    page = 0x1000
    payload_page = PAYLOAD_VA & ~(page - 1)
    payload_span = ((PAYLOAD_VA + len(payload) - payload_page + page - 1) // page) * page
    rejoin_page = ACTIVATE_REJOIN_VA & ~(page - 1)
    stack_page = 0x00100000
    stack_size = 0x00010000
    initial_sp = stack_page + stack_size // 2
    poison_lr = 0xDEADC0DC
    implementation = payload_symbols["guest_settings_activate_impl"][0]
    reached_rejoin = False
    instruction_count = 0

    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(payload_page, payload_span)
    uc.mem_write(PAYLOAD_VA, payload)
    # The semantic contract under test is the veneer itself.  Replace the C
    # body only inside Unicorn with an immediate return to model a bounded,
    # ABI-conforming callee.
    uc.mem_write(implementation, bytes.fromhex("1e ff 2f e1"))  # bx lr
    uc.mem_map(rejoin_page, page)
    uc.mem_write(ACTIVATE_REJOIN_VA, bytes.fromhex("00 f0 20 e3"))  # nop
    uc.mem_map(stack_page, stack_size)
    uc.reg_write(UC_ARM_REG_SP, initial_sp)
    uc.reg_write(UC_ARM_REG_LR, poison_lr)
    uc.reg_write(UC_ARM_REG_R5, 0x12345678)

    def on_code(engine: Uc, address: int, size: int, _user_data: object) -> None:
        nonlocal reached_rejoin, instruction_count
        instruction_count += 1
        if address == ACTIVATE_REJOIN_VA:
            reached_rejoin = True
            engine.emu_stop()

    uc.hook_add(UC_HOOK_CODE, on_code)
    try:
        uc.emu_start(PAYLOAD_VA, 0xFFFFFFFF, count=64)
    except UcError as error:
        raise AssertionError(f"activate veneer escaped before explicit rejoin: {error}") from error
    if not reached_rejoin:
        raise AssertionError("activate veneer did not reach explicit post-loop rejoin within 64 instructions")
    if instruction_count > 64:
        raise AssertionError("activate veneer exceeded instruction budget")
    if uc.reg_read(UC_ARM_REG_SP) != initial_sp:
        raise AssertionError("activate veneer did not restore SP")
    if uc.reg_read(UC_ARM_REG_LR) != poison_lr:
        raise AssertionError("activate veneer did not preserve incoming LR")
    if uc.reg_read(UC_ARM_REG_R0) != 0x001286BC:
        raise AssertionError("activate veneer did not replay the displaced literal load")


def emulate_veneer(
    payload: bytes,
    payload_symbols: dict[str, tuple[int, int, object]],
    entry_symbol: str,
    implementation_symbol: str,
    implementation_result: int | None,
    stop_addresses: set[int],
    initial_registers: dict[int, int],
    memory_words: dict[int, int] | None = None,
) -> tuple[Uc, int, int]:
    """Run one entry veneer with a bounded ABI-conforming C stub."""
    page = 0x1000
    payload_page = PAYLOAD_VA & ~(page - 1)
    payload_span = ((PAYLOAD_VA + len(payload) - payload_page + page - 1) // page) * page
    stack_page = 0x00100000
    stack_size = 0x00010000
    initial_sp = stack_page + stack_size // 2
    poison_lr = 0xDEADC0DC
    entry = payload_symbols[entry_symbol][0]
    implementation = payload_symbols[implementation_symbol][0]
    reached = 0
    instruction_count = 0
    mapped_pages = {payload_page}

    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(payload_page, payload_span)
    uc.mem_write(PAYLOAD_VA, payload)
    if implementation_result is None:
        stub = bytes.fromhex("1e ff 2f e1")  # bx lr
    else:
        if not 0 <= implementation_result <= 0xFF:
            raise AssertionError("veneer stub result is not ARM-immediate encodable")
        stub = struct.pack("<I", 0xE3A00000 | implementation_result) + \
               bytes.fromhex("1e ff 2f e1")
    uc.mem_write(implementation, stub)

    for address in sorted(stop_addresses | set((memory_words or {}).keys())):
        address_page = address & ~(page - 1)
        if address_page not in mapped_pages:
            uc.mem_map(address_page, page)
            mapped_pages.add(address_page)
    for address in stop_addresses:
        uc.mem_write(address, bytes.fromhex("00 f0 20 e3"))  # nop
    for address, value in (memory_words or {}).items():
        uc.mem_write(address, struct.pack("<I", value))

    uc.mem_map(stack_page, stack_size)
    uc.reg_write(UC_ARM_REG_SP, initial_sp)
    uc.reg_write(UC_ARM_REG_LR, poison_lr)
    for register, value in initial_registers.items():
        uc.reg_write(register, value)

    def on_code(engine: Uc, address: int, size: int, _user_data: object) -> None:
        nonlocal reached, instruction_count
        instruction_count += 1
        if address in stop_addresses:
            reached = address
            engine.emu_stop()

    uc.hook_add(UC_HOOK_CODE, on_code)
    try:
        uc.emu_start(entry, 0xFFFFFFFF, count=96)
    except UcError as error:
        raise AssertionError(
            f"{entry_symbol} escaped before an explicit rejoin: {error}"
        ) from error
    if reached == 0:
        raise AssertionError(f"{entry_symbol} did not reach a declared target")
    if instruction_count > 96:
        raise AssertionError(f"{entry_symbol} exceeded instruction budget")
    if uc.reg_read(UC_ARM_REG_SP) != initial_sp:
        raise AssertionError(f"{entry_symbol} did not restore SP")
    if uc.reg_read(UC_ARM_REG_LR) != poison_lr:
        raise AssertionError(f"{entry_symbol} did not preserve incoming LR")
    return uc, reached, instruction_count


def verify_navigation_veneers(
    payload: bytes, payload_symbols: dict[str, tuple[int, int, object]]
) -> None:
    """Prove all three new B-entered dispatchers rejoin only safe paths."""
    caller_values = {
        UC_ARM_REG_R0: 0x11111111,
        UC_ARM_REG_R1: 0x22222222,
        UC_ARM_REG_R2: 0x33333333,
        UC_ARM_REG_R3: 0x44444444,
        UC_ARM_REG_R12: 0xCCCCCCCC,
    }

    controls_object = 0x00200000
    controls_root = 0x23456000
    activate_registers = dict(caller_values)
    activate_registers[UC_ARM_REG_R11] = controls_object
    uc, reached, _ = emulate_veneer(
        payload,
        payload_symbols,
        "guest_settings_controls_activate_hook",
        "guest_settings_controls_activate_impl",
        None,
        {CONTROLS_ACTIVATE_REJOIN_VA},
        activate_registers,
        {controls_object + 0x1B8: controls_root},
    )
    if reached != CONTROLS_ACTIVATE_REJOIN_VA:
        raise AssertionError("controls activate veneer chose a wrong rejoin")
    if uc.reg_read(UC_ARM_REG_R0) != controls_root:
        raise AssertionError("controls activate veneer did not replay displaced ldr")
    for register in (UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R12):
        if uc.reg_read(register) != caller_values[register]:
            raise AssertionError("controls activate veneer clobbered a saved register")

    for result, expected_target in (
        (0, CONTROLS_CHECK_EPILOGUE_VA),
        (1, CONTROLS_CHECK_EVENT_CONTINUE_VA),
    ):
        check_registers = dict(caller_values)
        check_registers[UC_ARM_REG_R0] = 0xABCD0000 | result
        check_registers[UC_ARM_REG_R6] = 103
        check_registers[UC_ARM_REG_R8] = controls_object
        uc, reached, _ = emulate_veneer(
            payload,
            payload_symbols,
            "guest_settings_controls_check_dispatch",
            "guest_settings_controls_check_impl",
            result,
            {CONTROLS_CHECK_EVENT_CONTINUE_VA, CONTROLS_CHECK_EPILOGUE_VA},
            check_registers,
        )
        if reached != expected_target:
            raise AssertionError("controls check veneer chose a wrong branch")
        for register, value in check_registers.items():
            if uc.reg_read(register) != value:
                raise AssertionError("controls check veneer clobbered caller state")

    nav_cases = (
        (0, 110, OPTIONS_NAV_REJOIN_VA, 7, 110),
        (1, 104, OPTIONS_NAV_CONSUME_VA, caller_values[UC_ARM_REG_R0], 104),
        (2, 102, OPTIONS_NAV_REJOIN_VA, 0, 103),
    )
    for result, event_code, expected_target, expected_r0, expected_r8 in nav_cases:
        nav_registers = dict(caller_values)
        nav_registers[UC_ARM_REG_R4] = 0x00210000
        nav_registers[UC_ARM_REG_R8] = event_code
        uc, reached, _ = emulate_veneer(
            payload,
            payload_symbols,
            "guest_settings_options_nav_dispatch",
            "guest_settings_options_nav_impl",
            result,
            {OPTIONS_NAV_REJOIN_VA, OPTIONS_NAV_CONSUME_VA},
            nav_registers,
        )
        if reached != expected_target:
            raise AssertionError("options navigation veneer chose a wrong branch")
        if uc.reg_read(UC_ARM_REG_R0) != expected_r0 or \
                uc.reg_read(UC_ARM_REG_R8) != expected_r8:
            raise AssertionError("options navigation replay/redirect state mismatch")
        for register in (UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                         UC_ARM_REG_R4, UC_ARM_REG_R12):
            if uc.reg_read(register) != nav_registers[register]:
                raise AssertionError("options navigation veneer clobbered caller state")


def emulate_payload_function(
    payload: bytes,
    payload_symbols: dict[str, tuple[int, int, object]],
    function_name: str,
    arguments: tuple[int, ...],
    memory_words: dict[int, int],
    instruction_budget: int = 512,
) -> tuple[Uc, int]:
    """Execute one payload C function against a minimal synthetic guest heap."""
    page = 0x1000
    payload_page = PAYLOAD_VA & ~(page - 1)
    payload_span = ((PAYLOAD_VA + len(payload) - payload_page + page - 1) // page) * page
    stack_page = 0x00100000
    stack_size = 0x00010000
    initial_sp = stack_page + stack_size // 2
    return_sentinel = 0x00800000
    reached_return = False
    instruction_count = 0
    last_address = 0
    mapped_pages = set(range(payload_page, payload_page + payload_span, page))

    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(payload_page, payload_span)
    uc.mem_write(PAYLOAD_VA, payload)
    for address in sorted(set(memory_words) | {return_sentinel}):
        address_page = address & ~(page - 1)
        if address_page not in mapped_pages:
            uc.mem_map(address_page, page)
            mapped_pages.add(address_page)
    for address, value in memory_words.items():
        uc.mem_write(address, struct.pack("<I", value))
    uc.mem_write(return_sentinel, bytes.fromhex("00 f0 20 e3"))  # nop
    uc.mem_map(stack_page, stack_size)
    uc.reg_write(UC_ARM_REG_SP, initial_sp)
    uc.reg_write(UC_ARM_REG_LR, return_sentinel)
    uc.reg_write(UC_ARM_REG_C1_C0_2, 0x00F00000)
    uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
    for register, value in zip(
        (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3),
        arguments,
    ):
        uc.reg_write(register, value)

    def on_code(engine: Uc, address: int, size: int, _user_data: object) -> None:
        nonlocal reached_return, instruction_count, last_address
        instruction_count += 1
        last_address = address
        if address == return_sentinel:
            reached_return = True
            engine.emu_stop()

    uc.hook_add(UC_HOOK_CODE, on_code)
    try:
        uc.emu_start(payload_symbols[function_name][0], 0xFFFFFFFF,
                     count=instruction_budget)
    except UcError as error:
        raise AssertionError(
            f"{function_name} escaped synthetic guest at {last_address:#x}: {error}"
        ) from error
    if not reached_return:
        raise AssertionError(f"{function_name} exceeded its instruction budget")
    if uc.reg_read(UC_ARM_REG_SP) != initial_sp:
        raise AssertionError(f"{function_name} did not restore SP")
    return uc, instruction_count


def verify_c_dispatch_semantics(
    payload: bytes, payload_symbols: dict[str, tuple[int, int, object]]
) -> None:
    """Exercise the actual compiled C guards for IDs 13/14/15."""
    controls = 0x00500000
    controls_tab = 0x00501000
    options = 0x00502000
    options_tab = 0x00503000
    valid_state = {
        STATE_VA + 0x000: 0x42545347,
        STATE_VA + 0x004: 8,
        STATE_VA + 0x008: 0x400,
    }

    for event_code, tab_event, requested in ((103, 0, 15), (0, 1, 14)):
        memory = dict(valid_state)
        memory.update({
            controls + 0x1BC: controls_tab,
            controls_tab + 0x37C: 15,
            controls_tab + 0x380: requested,
        })
        uc, _ = emulate_payload_function(
            payload,
            payload_symbols,
            "guest_settings_controls_check_impl",
            (controls, event_code, tab_event, PAYLOAD_VA),
            memory,
        )
        if uc.reg_read(UC_ARM_REG_R0) != 1:
            raise AssertionError("compiled Controls custom redirect was not handled")
        if u32(uc.mem_read(PENDING_CUSTOM_VA, 4), 0) != PENDING_CUSTOM_MAGIC:
            raise AssertionError("compiled Controls redirect missed reserved[1]")
        if u32(uc.mem_read(controls_tab + 0x37C, 4), 0) != 15 or \
                u32(uc.mem_read(controls_tab + 0x380, 4), 0) != 13:
            raise AssertionError("compiled Controls redirect did not fail safe to ID13")

    passthrough_memory = dict(valid_state)
    passthrough_memory.update({
        controls + 0x1BC: controls_tab,
        controls_tab + 0x37C: 15,
        controls_tab + 0x380: 13,
    })
    uc, _ = emulate_payload_function(
        payload,
        payload_symbols,
        "guest_settings_controls_check_impl",
        (controls, 0, 1, PAYLOAD_VA),
        passthrough_memory,
    )
    if uc.reg_read(UC_ARM_REG_R0) != 1 or \
            u32(uc.mem_read(PENDING_CUSTOM_VA, 4), 0) != 0:
        raise AssertionError("compiled Controls ordinary event did not pass through")

    # Invalid protocol state plus requested 14 must still be consumed locally.
    fail_safe_memory = {
        STATE_VA: 0,
        options + 0x1BC: options_tab,
        options_tab + 0x370: 0,
        options_tab + 0x374: 0,
        options_tab + 0x37C: 14,
        options_tab + 0x380: 14,
    }
    uc, _ = emulate_payload_function(
        payload,
        payload_symbols,
        "guest_settings_handle_event",
        (options, PAYLOAD_VA),
        fail_safe_memory,
    )
    if uc.reg_read(UC_ARM_REG_R0) != 1 or \
            u32(uc.mem_read(options_tab + 0x37C, 4), 0) != 13 or \
            u32(uc.mem_read(options_tab + 0x380, 4), 0) != 13:
        raise AssertionError("compiled Options ID14 fail-safe did not repair to ID13")

    custom_rb_memory = dict(valid_state)
    custom_rb_memory.update({
        STATE_VA + 0x00C: 3,
        STATE_VA + 0x030: options,
        STATE_VA + 0x038: options_tab,
    })
    uc, _ = emulate_payload_function(
        payload,
        payload_symbols,
        "guest_settings_options_nav_impl",
        (options, 103, PAYLOAD_VA),
        custom_rb_memory,
    )
    if uc.reg_read(UC_ARM_REG_R0) != 1:
        raise AssertionError("compiled custom-active RB was not consumed")


def verify_focus_lifetime_semantics(
    payload: bytes, payload_symbols: dict[str, tuple[int, int, object]]
) -> None:
    """Prove enter/tick/leave call the native focus setter with (manager, NULL)."""
    options = 0x00500000
    root = 0x00501000
    tab = 0x00502000
    child_vector = 0x00503000
    custom = 0x00504000
    stale_widget = 0x00505000
    bx_lr = 0xE12FFF1E
    # Synthetic implementation of the proven native ABI:
    #   r0 = rglWindow_manager*, r1 = rglWidget_base*
    # It records r1 in the original manager's active-widget slot.
    focus_stub = {
        SET_ACTIVE_WIDGET_HIGHLIGHT_VA: 0xE5801AD4,
        SET_ACTIVE_WIDGET_HIGHLIGHT_VA + 4: bx_lr,
        RGL_WINDOW_MANAGER_VA + RGL_WINDOW_MANAGER_ACTIVE_OFFSET: stale_widget,
    }
    valid_state = {
        STATE_VA + 0x000: 0x42545347,
        STATE_VA + 0x004: 8,
        STATE_VA + 0x008: 0x400,
        STATE_VA + 0x030: options,
        STATE_VA + 0x034: root,
        STATE_VA + 0x038: tab,
        STATE_VA + 0x03C: custom,
    }

    enter_memory = dict(valid_state)
    enter_memory.update(focus_stub)
    enter_memory.update({
        STATE_VA + 0x00C: 2,
        options + 0x1BC: tab,
        tab + 0x36C: 0,
        tab + 0x370: 0,
        tab + 0x374: 0,
        tab + 0x37C: 13,
        tab + 0x380: 14,
        root + 0x35C: child_vector,
        root + 0x360: child_vector,
        WIDGET_UNHIDE_VA: bx_lr,
        WIDGET_UNDISABLE_VA: bx_lr,
    })
    uc, _ = emulate_payload_function(
        payload,
        payload_symbols,
        "guest_settings_handle_event",
        (options, PAYLOAD_VA),
        enter_memory,
        instruction_budget=2048,
    )
    if u32(uc.mem_read(
        RGL_WINDOW_MANAGER_VA + RGL_WINDOW_MANAGER_ACTIVE_OFFSET, 4
    ), 0) != 0:
        raise AssertionError("compiled custom enter did not clear native active focus")
    if u32(uc.mem_read(STATE_VA + 0x00C, 4), 0) != 3 or \
            u32(uc.mem_read(tab + 0x37C, 4), 0) != 14:
        raise AssertionError("focus-tested custom enter did not become active")

    tick_memory = dict(valid_state)
    tick_memory.update(focus_stub)
    tick_memory[STATE_VA + 0x00C] = 3
    uc, _ = emulate_payload_function(
        payload,
        payload_symbols,
        "guest_settings_tick",
        (options, PAYLOAD_VA),
        tick_memory,
        instruction_budget=1024,
    )
    if u32(uc.mem_read(
        RGL_WINDOW_MANAGER_VA + RGL_WINDOW_MANAGER_ACTIVE_OFFSET, 4
    ), 0) != 0:
        raise AssertionError("compiled custom tick did not suppress stale focus")

    leave_memory = dict(valid_state)
    leave_memory.update(focus_stub)
    leave_memory.update({
        STATE_VA + 0x00C: 3,
        tab + 0x370: 0,
        tab + 0x374: 0,
        tab + 0x37C: 14,
        tab + 0x380: 14,
        WIDGET_HIDE_VA: bx_lr,
        WIDGET_DISABLE_VA: bx_lr,
    })
    uc, _ = emulate_payload_function(
        payload,
        payload_symbols,
        "guest_settings_options_nav_impl",
        (options, 102, PAYLOAD_VA),
        leave_memory,
        instruction_budget=2048,
    )
    if u32(uc.mem_read(
        RGL_WINDOW_MANAGER_VA + RGL_WINDOW_MANAGER_ACTIVE_OFFSET, 4
    ), 0) != 0:
        raise AssertionError("compiled custom leave retained a dangling focus")
    if uc.reg_read(UC_ARM_REG_R0) != 2 or \
            u32(uc.mem_read(STATE_VA + 0x00C, 4), 0) != 2 or \
            u32(uc.mem_read(tab + 0x37C, 4), 0) != 13 or \
            u32(uc.mem_read(tab + 0x380, 4), 0) != 15:
        raise AssertionError("focus-tested leave did not restore native navigation route")


def file_offset(elf: ELFFile, va: int, size: int) -> int:
    for segment in elf.iter_segments():
        if segment["p_type"] != "PT_LOAD":
            continue
        start = int(segment["p_vaddr"])
        filesz = int(segment["p_filesz"])
        if start <= va and va + size <= start + filesz:
            return int(segment["p_offset"]) + va - start
    raise AssertionError(f"VA {va:#x}+{size:#x} is not file-backed")


def symbols(elf: ELFFile, section_name: str) -> dict[str, tuple[int, int, object]]:
    section = elf.get_section_by_name(section_name)
    if section is None:
        raise AssertionError(f"missing {section_name}")
    return {
        symbol.name: (int(symbol["st_value"]), int(symbol["st_size"]), symbol["st_shndx"])
        for symbol in section.iter_symbols()
        if symbol.name
    }


def verify_no_absolute_jump_table(payload: bytes,
                                  payload_symbols: dict[str, tuple[int, int, object]]) -> None:
    """Reject the exact non-PIE jump-table shape that loses the runtime load bias."""
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN)
    ranges = [
        (name, address, size)
        for name, (address, size, _section) in payload_symbols.items()
        if name.startswith("guest_settings_") and size > 0
    ]
    if not ranges:
        raise AssertionError("no guest_settings functions available for jump-table proof")
    for name, address, size in ranges:
        if address < PAYLOAD_VA or address + size > PAYLOAD_VA + len(payload):
            raise AssertionError(f"invalid executable symbol extent for {name}")
        start = address - PAYLOAD_VA
        for instruction in md.disasm(payload[start:start + size], address):
            if instruction.mnemonic == "ldr" and instruction.op_str.startswith("pc, "):
                raise AssertionError(
                    f"load-bias-unsafe absolute jump table in {name} at "
                    f"{instruction.address:#x}: {instruction.mnemonic} {instruction.op_str}"
                )


def verify_source_layout_contract() -> None:
    """Reject source architectures that cannot match the native Options tree."""
    source_path = Path(__file__).with_name("guest_settings_tab.c")
    source = source_path.read_text(encoding="utf-8")
    compact = re.sub(r"\s+", " ", source)

    removed_camera_selector_tokens = (
        "Camera input mode",
        "Camera look method",
        "Right stick / Mouse",
        "str_camera_look",
        "create_camera_look_combo",
    )
    for token in removed_camera_selector_tokens:
        if token in source:
            raise AssertionError(f"removed camera selector token remains: {token}")

    removed_layout_setting_tokens = (
        "mb_layout_edit_check_box",
        "str_en_layout_edit",
        "str_ru_layout_edit",
        "GST_RESERVED_LAYOUT_EDIT_PENDING",
        "maybe_launch_layout_edit_battle",
    )
    for token in removed_layout_setting_tokens:
        if token in source:
            raise AssertionError(f"removed layout-setting token remains: {token}")

    expected_macros = {
        "VA_CREATE_VERTICAL_SCROLLABLE": "0x002A83C0u",
        "VA_UPDATE_SCROLLABLE_BOARD": "0x002A9690u",
        "VA_SET_ACTIVE_WIDGET_HIGHLIGHT": "0x00214E94u",
        "VA_RGL_WINDOW_MANAGER": "0x003E6188u",
        "OPTIONS_LOGO_OFFSET": "0x01C0u",
        "OPTIONS_DONE_OFFSET": "0x0264u",
        "TAB_PANEL_OFFSET": "0x036Cu",
        "VERTICAL_CONTENT_OFFSET": "0x0748u",
    }
    for name, value in expected_macros.items():
        pattern = rf"^\s*#define\s+{re.escape(name)}\s+{re.escape(value)}\s*$"
        if re.search(pattern, source, re.MULTILINE) is None:
            raise AssertionError(f"source layout macro {name} must be {value}")

    forbidden_identifiers = (
        "VA_OPERATOR_NEW",
        "VA_CONTAINER_CTOR",
        "VA_WIDGET_SET_POSITION",
        "VA_WIDGET_SET_SIZE",
        "VA_CONTAINER_ADD_CHILD",
        "VA_CREATE_TAB_PANEL",
        "RGL_CONTAINER_BYTES",
        "gst_new_fn",
        "gst_container_ctor_fn",
        "gst_add_child_fn",
        "gst_create_tab_panel_fn",
    )
    for identifier in forbidden_identifiers:
        if identifier in source:
            raise AssertionError(f"forbidden legacy layout primitive remains: {identifier}")

    required_snippets = (
        "state->custom_container = outer;",
        "set_widget_inactive(base, outer, 1);",
        "content = load_word(outer + VERTICAL_CONTENT_OFFSET);",
        "frame = load_word(tab + TAB_PANEL_OFFSET);",
        "state->custom_frame = frame;",
        "load_word(options_window + OPTIONS_LOGO_OFFSET) : 0u;",
        "load_word(options_window + OPTIONS_DONE_OFFSET) : 0u;",
        "child == tab || child == custom || child == logo || child == done || child == frame",
        "((gst_widget_void_fn)runtime_va(base, VA_UPDATE_SCROLLABLE_BOARD))( (void*)outer);",
        "(void*)runtime_va(base, VA_RGL_WINDOW_MANAGER), (void*)0);",
    )
    for snippet in required_snippets:
        if snippet not in compact:
            raise AssertionError(f"missing native layout source contract: {snippet}")

    vertical_pattern = re.compile(
        r"outer\s*=\s*\(gst_u32\)\(\(gst_create_vertical_scrollable_fn\)runtime_va\(\s*"
        r"base,\s*VA_CREATE_VERTICAL_SCROLLABLE\)\)\(\s*\(void\*\)root,\s*&temporary,\s*"
        r"&outer_x,\s*&outer_y,\s*&outer_w,\s*&outer_h\s*\);",
        re.DOTALL,
    )
    if vertical_pattern.search(source) is None:
        raise AssertionError("vertical-scrollable factory ABI/source call drift")

    wrapper_patterns = (
        r"create_checkbox\(gst_u32 base, gst_u32 parent, const char\* internal_name, "
        r"const char\* visible_text, float x, float y, float scale\)",
        r"create_text\(gst_u32 base, gst_u32 parent, const char\* internal_name, "
        r"const char\* visible_text, float x, float y, float scale\)",
        r"create_slider\(gst_u32 base, gst_u32 parent, const char\* internal_name, "
        r"float x, float y, float scale\)",
    )
    for pattern in wrapper_patterns:
        if re.search(pattern, compact) is None:
            raise AssertionError(f"widget wrapper ABI/source signature drift: {pattern}")

    for internal_name in (
        *CHECKBOX_INTERNAL_NAMES,
        *TEXT_INTERNAL_NAMES,
        *SLIDER_INTERNAL_NAMES,
        *COMBO_INTERNAL_NAMES,
    ):
        if source.count(f'"{internal_name}"') != 1:
            raise AssertionError(f"internal name must have one stable declaration: {internal_name}")

    vertical_at = source.index("outer = (gst_u32)((gst_create_vertical_scrollable_fn)")
    inactive_at = source.index("set_widget_inactive(base, outer, 1);")
    first_child_at = source.index("state->checkboxes[2u] = create_checkbox(")
    last_slider_at = source.index(
        "state->sliders[GST_FLOAT_GYRO_DEADZONE] = create_slider(")
    update_at = source.index("VA_UPDATE_SCROLLABLE_BOARD", last_slider_at)
    if not vertical_at < inactive_at < first_child_at < last_slider_at < update_at:
        raise AssertionError("outer hide/child/update construction order drift")

    focus_call = "clear_active_widget_highlight(base);"
    if source.count(focus_call) != 3:
        raise AssertionError("focus clear must exist exactly at enter, leave, and active tick")
    show_at = source.index("static int show_custom")
    leave_at = source.index("static void leave_custom", show_at)
    tick_at = source.index("void guest_settings_tick")
    show_focus_at = source.index(focus_call, show_at, leave_at)
    show_snapshot_at = source.index("snapshot_and_hide_siblings(state)", show_at, leave_at)
    show_unhide_at = source.index(
        "set_widget_inactive(base, state->custom_container, 0)", show_at, leave_at
    )
    if not show_snapshot_at < show_focus_at < show_unhide_at:
        raise AssertionError("custom enter must clear focus after commit and before reveal")
    leave_end = source.index("void guest_settings_controls_activate_impl", leave_at)
    leave_capture_at = source.index("capture_values(base, state)", leave_at, leave_end)
    leave_focus_at = source.index(focus_call, leave_at, leave_end)
    leave_hide_at = source.index(
        "set_widget_inactive(base, state->custom_container, 1)", leave_at, leave_end
    )
    if not leave_capture_at < leave_focus_at < leave_hide_at:
        raise AssertionError("custom leave must clear focus before hiding custom widgets")
    tick_focus_at = source.index(focus_call, tick_at)
    tick_apply_at = source.index("apply_values(base, state)", tick_at)
    if not tick_at < tick_focus_at < tick_apply_at:
        raise AssertionError("custom active tick must suppress focus before widget updates")


def read_c_string(engine: Uc, address: int, limit: int = 256) -> str:
    data = bytearray()
    for offset in range(limit):
        value = bytes(engine.mem_read(address + offset, 1))[0]
        if value == 0:
            return data.decode("utf-8")
        data.append(value)
    raise AssertionError(f"unterminated runtime string at {address:#x}")


def read_float(engine: Uc, address: int) -> float:
    return struct.unpack("<f", bytes(engine.mem_read(address, 4)))[0]


def assert_float(actual: float, expected: float, context: str) -> None:
    if abs(actual - expected) > 1.0e-6:
        raise AssertionError(f"{context}: got {actual!r}, expected {expected!r}")


def verify_layout_construction(
    payload: bytes, payload_symbols: dict[str, tuple[int, int, object]]
) -> None:
    """Execute the builder against stubs and prove its full native ABI trace."""

    def run_case(russian: bool) -> None:
        page = 0x1000
        payload_page = PAYLOAD_VA & ~(page - 1)
        payload_span = (
            (PAYLOAD_VA + len(payload) - payload_page + page - 1) // page
        ) * page
        stack_page = 0x00100000
        stack_size = 0x00010000
        initial_sp = stack_page + stack_size // 2
        return_sentinel = 0x00800000
        options = 0x00500000
        root = 0x00501000
        tab = 0x00502000
        frame = 0x00503000
        logo = 0x00504000
        done = 0x00505000
        outer = 0x00600000
        content = 0x00603000
        widget_base = 0x00610000
        next_widget = widget_base
        instruction_count = 0
        reached_return = False
        last_address = 0

        engine = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        engine.mem_map(payload_page, payload_span)
        engine.mem_write(PAYLOAD_VA, payload)
        mapped_pages = set(range(payload_page, payload_page + payload_span, page))

        def map_region(address: int, size: int) -> None:
            start = address & ~(page - 1)
            end = (address + size + page - 1) & ~(page - 1)
            for page_address in range(start, end, page):
                if page_address not in mapped_pages:
                    engine.mem_map(page_address, page)
                    mapped_pages.add(page_address)

        def write_u32(address: int, value: int) -> None:
            engine.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))

        def register(register: int) -> int:
            return int(engine.reg_read(register))

        def stack_word(offset: int) -> int:
            return u32(bytes(engine.mem_read(register(UC_ARM_REG_SP) + offset, 4)), 0)

        def object_string(objects: dict[int, str], object_address: int) -> str:
            if object_address not in objects:
                raise AssertionError(
                    f"factory received unknown rglString object {object_address:#x}"
                )
            return objects[object_address]

        map_region(stack_page, stack_size)
        map_region(STATE_VA, STATE_END_VA - STATE_VA)
        for address in (options, root, tab, frame, logo, done):
            map_region(address, page)
        map_region(outer, 0x5000)
        map_region(widget_base, 0x20000)
        map_region(LANGUAGE_LITERAL_VA, 4)
        map_region(return_sentinel, 4)

        expected_stub_vas = {
            STRING_CTOR_VA,
            STRING_DTOR_VA,
            CREATE_TEXT_VA,
            CREATE_CHECK_BOX_VA,
            WIDGET_HIDE_VA,
            WIDGET_UNHIDE_VA,
            WIDGET_DISABLE_VA,
            WIDGET_UNDISABLE_VA,
            TAB_ADD_ITEM_VA,
            CREATE_VERTICAL_SCROLLABLE_VA,
            CREATE_SLIDER_HOR_VA,
            CREATE_COMBO_LABEL_VA,
            COMBO_ADD_ITEM_VA,
            COMBO_SET_VALUE_I_VA,
            UPDATE_SCROLLABLE_BOARD_VA,
            CHECK_BOX_SET_VALUE_I_VA,
            WIDGET_SET_VALUE_F_VA,
            SET_ACTIVE_WIDGET_HIGHLIGHT_VA,
        }
        all_stub_vas = expected_stub_vas | set(FORBIDDEN_LAYOUT_VAS)
        for address in all_stub_vas:
            map_region(address, 4)
            engine.mem_write(address, bytes.fromhex("1e ff 2f e1"))  # bx lr
        engine.mem_write(return_sentinel, bytes.fromhex("00 f0 20 e3"))  # nop

        language_word = 0x1000 if russian else EXPECTED_LANGUAGE_WORD
        write_u32(LANGUAGE_LITERAL_VA, language_word)
        if russian:
            language_address = LANGUAGE_GOT_BASE + language_word
            map_region(language_address, 3)
            engine.mem_write(language_address, b"ru\0")

        write_u32(options + 0x1B8, root)
        write_u32(options + 0x1BC, tab)
        write_u32(options + 0x1C0, logo)
        write_u32(options + 0x264, done)
        write_u32(tab + 0x36C, frame)
        write_u32(tab + 0x370, 0)
        write_u32(tab + 0x374, 0)
        write_u32(outer + 0x748, content)

        string_objects: dict[int, str] = {}
        tab_items: list[tuple[int, str, int]] = []
        outer_calls: list[tuple[int, str, float, float, float, float]] = []
        controls: list[tuple[str, int, str, str | None, float, float, float, int]] = []
        checkboxes: list[int] = []
        sliders: list[int] = []
        update_calls: list[int] = []
        combo_options: list[str] = []
        layout_events: list[str] = []
        forbidden_calls: list[str] = []

        def allocate_widget() -> int:
            nonlocal next_widget
            result = next_widget
            next_widget += page
            return result

        def record_text_or_checkbox(kind: str) -> int:
            result = allocate_widget()
            parent = register(UC_ARM_REG_R0)
            name = object_string(string_objects, register(UC_ARM_REG_R1))
            visible = object_string(string_objects, register(UC_ARM_REG_R2))
            x = read_float(engine, register(UC_ARM_REG_R3))
            y = read_float(engine, stack_word(0))
            scale = read_float(engine, stack_word(4))
            controls.append((kind, parent, name, visible, x, y, scale, result))
            layout_events.append(kind)
            return result

        def on_code(current: Uc, address: int, size: int, _user_data: object) -> None:
            nonlocal instruction_count, reached_return, last_address
            instruction_count += 1
            last_address = address
            if address == return_sentinel:
                reached_return = True
                current.emu_stop()
                return
            if address in FORBIDDEN_LAYOUT_VAS:
                forbidden_calls.append(FORBIDDEN_LAYOUT_VAS[address])
                if address == 0x00352B90:
                    current.reg_write(UC_ARM_REG_R0, outer)
                elif address == 0x0029EF00:
                    current.reg_write(UC_ARM_REG_R0, frame)
                return
            if address == STRING_CTOR_VA:
                string_objects[register(UC_ARM_REG_R0)] = read_c_string(
                    current, register(UC_ARM_REG_R1)
                )
            elif address == STRING_DTOR_VA:
                string_objects.pop(register(UC_ARM_REG_R0), None)
            elif address == TAB_ADD_ITEM_VA:
                tab_items.append((
                    register(UC_ARM_REG_R0),
                    object_string(string_objects, register(UC_ARM_REG_R1)),
                    register(UC_ARM_REG_R2),
                ))
            elif address == CREATE_VERTICAL_SCROLLABLE_VA:
                outer_calls.append((
                    register(UC_ARM_REG_R0),
                    object_string(string_objects, register(UC_ARM_REG_R1)),
                    read_float(current, register(UC_ARM_REG_R2)),
                    read_float(current, register(UC_ARM_REG_R3)),
                    read_float(current, stack_word(0)),
                    read_float(current, stack_word(4)),
                ))
                layout_events.append("vertical")
                current.reg_write(UC_ARM_REG_R0, outer)
            elif address == WIDGET_HIDE_VA:
                if register(UC_ARM_REG_R0) == outer:
                    layout_events.append("hide")
            elif address == WIDGET_DISABLE_VA:
                if register(UC_ARM_REG_R0) == outer:
                    layout_events.append("disable")
            elif address == CREATE_CHECK_BOX_VA:
                result = record_text_or_checkbox("checkbox")
                checkboxes.append(result)
                current.reg_write(UC_ARM_REG_R0, result)
            elif address == CREATE_TEXT_VA:
                result = record_text_or_checkbox("text")
                current.reg_write(UC_ARM_REG_R0, result)
            elif address == CREATE_SLIDER_HOR_VA:
                result = allocate_widget()
                controls.append((
                    "slider",
                    register(UC_ARM_REG_R0),
                    object_string(string_objects, register(UC_ARM_REG_R1)),
                    None,
                    read_float(current, register(UC_ARM_REG_R2)),
                    read_float(current, register(UC_ARM_REG_R3)),
                    read_float(current, stack_word(0)),
                    result,
                ))
                sliders.append(result)
                layout_events.append("slider")
                current.reg_write(UC_ARM_REG_R0, result)
            elif address == CREATE_COMBO_LABEL_VA:
                result = allocate_widget()
                combo_name = object_string(
                    string_objects, register(UC_ARM_REG_R1))
                controls.append((
                    "combo",
                    register(UC_ARM_REG_R0),
                    combo_name,
                    None,
                    read_float(current, register(UC_ARM_REG_R2)),
                    read_float(current, register(UC_ARM_REG_R3)),
                    read_float(current, stack_word(0)),
                    result,
                ))
                sliders.append(result)
                layout_events.append("combo")
                current.reg_write(UC_ARM_REG_R0, result)
            elif address == COMBO_ADD_ITEM_VA:
                combo_options.append(
                    object_string(string_objects, register(UC_ARM_REG_R1)))
            elif address == COMBO_SET_VALUE_I_VA:
                write_u32(register(UC_ARM_REG_R0) + 0x2B0, register(UC_ARM_REG_R1))
            elif address == UPDATE_SCROLLABLE_BOARD_VA:
                update_calls.append(register(UC_ARM_REG_R0))
                layout_events.append("update")
            elif address == CHECK_BOX_SET_VALUE_I_VA:
                write_u32(register(UC_ARM_REG_R0) + 0x2B0, register(UC_ARM_REG_R1))
            elif address == WIDGET_SET_VALUE_F_VA:
                write_u32(register(UC_ARM_REG_R0) + 0x2BC, register(UC_ARM_REG_R1))

        engine.hook_add(UC_HOOK_CODE, on_code)
        engine.reg_write(UC_ARM_REG_SP, initial_sp)
        engine.reg_write(UC_ARM_REG_LR, return_sentinel)
        engine.reg_write(UC_ARM_REG_R0, options)
        engine.reg_write(UC_ARM_REG_R1, PAYLOAD_VA)
        engine.reg_write(UC_ARM_REG_C1_C0_2, 0x00F00000)
        engine.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        try:
            engine.emu_start(
                payload_symbols["guest_settings_activate_impl"][0],
                0xFFFFFFFF,
                count=40000,
            )
        except UcError as error:
            raise AssertionError(
                f"layout builder escaped synthetic guest at {last_address:#x}: {error}"
            ) from error
        if not reached_return:
            raise AssertionError("layout builder exceeded its instruction budget")
        if engine.reg_read(UC_ARM_REG_SP) != initial_sp:
            raise AssertionError("layout builder did not restore SP")
        if forbidden_calls:
            raise AssertionError(
                "layout builder called forbidden primitive(s): "
                + ", ".join(forbidden_calls)
            )

        visible = VISIBLE_RU if russian else VISIBLE_EN
        if tab_items != [(tab, visible[0], 14)]:
            raise AssertionError(f"custom tab header ABI/text drift: {tab_items!r}")
        if len(outer_calls) != 1:
            raise AssertionError(f"expected one vertical factory call, got {outer_calls!r}")
        outer_parent, outer_name, outer_x, outer_y, outer_w, outer_h = outer_calls[0]
        if outer_parent != root or outer_name != "mb_guest_settings":
            raise AssertionError("vertical factory parent/internal name drift")
        for actual, expected, field in (
            (outer_x, 0.0, "outer x"),
            (outer_y, 0.05, "outer y"),
            (outer_w, 0.76, "outer width"),
            (outer_h, 0.60, "outer height"),
        ):
            assert_float(actual, expected, field)

        expected_controls: list[tuple[str, str, str | None, float, float, float]] = []
        # Creation order mirrors guest_settings_activate_impl exactly.
        creation = (
            ("checkbox", 0), ("checkbox", 1),
            ("text", 0), ("slider", 0),
            ("checkbox", 2), ("checkbox", 3),
            ("checkbox", 4), ("checkbox", 5), ("checkbox", 6),
            ("text", 1), ("slider", 1),
            ("text", 2), ("slider", 2),
            ("text", 3), ("slider", 3),
            ("text", 4), ("slider", 4),
            ("text", 5), ("combo", 0),
            ("checkbox", 7), ("text", 6), ("slider", 5), ("checkbox", 8),
            ("checkbox", 9), ("checkbox", 10),
            ("text", 7), ("slider", 6),
            ("text", 8), ("slider", 7),
        )
        checkbox_visible_used = 0
        textslider_used = 0
        for kind, index in creation:
            if kind == "checkbox":
                name, x, y, scale = CHECKBOX_LAYOUT[index]
                expected_controls.append((
                    "checkbox", name, visible[CHECKBOX_VISIBLE[checkbox_visible_used]],
                    x, y, scale,
                ))
                checkbox_visible_used += 1
            elif kind == "combo":
                combo_name, combo_x, combo_y, combo_scale = COMBO_LAYOUT[index]
                expected_controls.append((
                    "combo", combo_name, None, combo_x, combo_y, combo_scale,
                ))
                textslider_used += 1
            elif kind == "text":
                text_name, text_x, text_y, text_scale = TEXT_LAYOUT[index]
                label_index = TEXTSLIDER_LABELS[textslider_used]
                expected_controls.append((
                    "text", text_name, visible[label_index],
                    text_x, text_y, text_scale,
                ))
            else:
                slider_name, slider_x, slider_y, slider_scale = SLIDER_LAYOUT[index]
                expected_controls.append((
                    "slider", slider_name, None, slider_x, slider_y, slider_scale,
                ))
                textslider_used += 1

        if len(controls) != len(expected_controls):
            raise AssertionError(
                f"widget factory count drift: got {len(controls)}, "
                f"expected {len(expected_controls)}"
            )
        for index, (actual, expected) in enumerate(zip(controls, expected_controls)):
            kind, parent, name, human, x, y, scale, _result = actual
            expected_kind, expected_name, expected_human, expected_x, expected_y, \
                expected_scale = expected
            if (kind, parent, name, human) != (
                expected_kind, content, expected_name, expected_human
            ):
                raise AssertionError(
                    f"widget {index} ABI/name/parent drift: {actual!r}"
                )
            assert_float(x, expected_x, f"widget {index} x")
            assert_float(y, expected_y, f"widget {index} y")
            assert_float(scale, expected_scale, f"widget {index} scale")

        expected_events = ["vertical", "hide", "disable"]
        expected_events.extend(kind for kind, _index in creation)
        expected_events.append("update")
        # The post-build visibility passes can collapse gyro and HDR-dependent
        # rows, each producing one additional board update.
        if not any(layout_events == expected_events + ["update"] * extra
                   for extra in range(3)):
            raise AssertionError(f"layout construction order drift: {layout_events!r}")
        expected_max_choice = "макс." if russian else "max."
        if combo_options != ["30", "60", "90", expected_max_choice]:
            raise AssertionError(f"combo choices drift: {combo_options!r}")
        if sorted(update_calls) != [outer] * len(update_calls):
            raise AssertionError(f"board update target/count drift: {update_calls!r}")

        state = bytes(engine.mem_read(STATE_VA, STATE_END_VA - STATE_VA))
        if u32(state, 0x03C) != outer:
            raise AssertionError("custom_container does not retain vertical outer")
        if u32(state, 0x040) != frame:
            raise AssertionError("custom_frame does not retain tab+0x36C panel")
        if (u32(state, 0x074) & 0x40) != 0 or (u32(state, 0x090) & 0x40) != 0:
            raise AssertionError("camera input must default to Right stick")
        # Core protocol slots remain v2-stable; graphics widget pointers use
        # named reserved[] extension slots.
        creation_to_protocol = (8, 10, 13, 14, 2, 11, 4, 12, 0, 3, 1)
        expected_checkbox_state = [
            checkboxes[creation_to_protocol.index(index)]
            if index in creation_to_protocol else 0
            for index in range(6)
        ]
        if [u32(state, 0x044 + index * 4) for index in range(6)] != expected_checkbox_state:
            raise AssertionError("checkbox state pointers do not match factory returns")
        slider_creation_to_protocol = (6, 4, 3, 0, 8, 5, 9, 1, 2)
        if [u32(state, 0x05C + index * 4) for index in range(6)] != [
                sliders[slider_creation_to_protocol.index(index)] for index in range(6)]:
            raise AssertionError("slider state pointers do not match factory returns")
        if u32(state, 0x2C4) != checkboxes[0] or \
                u32(state, 0x2C8) != 0 or \
                u32(state, 0x2CC) != checkboxes[1]:
            raise AssertionError("graphics checkbox extension pointers drift")
        if u32(state, 0x304) != checkboxes[5]:
            raise AssertionError("haptic checkbox extension pointer drift")
        if u32(state, 0x328) != checkboxes[7]:
            raise AssertionError("menu aspect checkbox extension pointer drift")
        if u32(state, 0x32C) != checkboxes[2]:
            raise AssertionError("FXAA checkbox extension pointer drift")
        if u32(state, 0x2D4) != sliders[0] or \
                u32(state, 0x2D8) != 0 or u32(state, 0x2DC) != checkboxes[3]:
            raise AssertionError("graphics widget extension pointers drift")
        if u32(state, 0x320) != controls[15][7] or u32(state, 0x324) != sliders[4]:
            raise AssertionError("map-pan widget extension pointers drift")
        if u32(state, 0x338) != controls[20][7] or u32(state, 0x33C) != sliders[6]:
            raise AssertionError("report-scale widget extension pointers drift")
        assert_float(struct.unpack_from("<f", state, 0x330)[0], 1.0,
                     "report-scale host default")
        assert_float(struct.unpack_from("<f", state, 0x334)[0], 1.0,
                     "report-scale captured default")
        if u32(state, 0x00C) != 2 or u32(state, 0x010) != 0:
            raise AssertionError("successful layout build did not reach READY/no-error")

        # Execute the real extension float path: host preference -> slider,
        # followed by a player change -> published guest snapshot.
        write_u32(STATE_VA + 0x00C, 3)
        write_u32(STATE_VA + 0x024, 4)
        engine.mem_write(STATE_VA + 0x330, struct.pack("<f", 0.75))

        def settings_tick() -> None:
            nonlocal reached_return
            reached_return = False
            engine.reg_write(UC_ARM_REG_R0, options)
            engine.reg_write(UC_ARM_REG_R1, PAYLOAD_VA)
            engine.reg_write(UC_ARM_REG_LR, return_sentinel)
            engine.emu_start(payload_symbols["guest_settings_tick"][0],
                             0xFFFFFFFF, count=10000)
            if not reached_return or register(UC_ARM_REG_SP) != initial_sp:
                raise AssertionError("settings tick did not return with balanced SP")

        settings_tick()
        assert_float(read_float(engine, sliders[6] + 0x2BC), 0.25,
                     "report-scale host seed to normalized slider")
        assert_float(read_float(engine, STATE_VA + 0x334), 0.75,
                     "report-scale host seed to guest snapshot")
        engine.mem_write(sliders[6] + 0x2BC, struct.pack("<f", 0.9))
        settings_tick()
        assert_float(read_float(engine, STATE_VA + 0x334), 1.4,
                     "report-scale player change to guest snapshot")
        assert_float(read_float(engine, STATE_VA + 0x318), 0.70,
                     "report-scale does not overwrite map-pan extension")
        if u32(bytes(engine.mem_read(STATE_VA, 0x400)), 0x02C) & 1:
            raise AssertionError("report-scale snapshot was left unpublished")

    run_case(False)
    run_case(True)


def payload_proof(elf_path: Path, bin_path: Path) -> dict[str, object]:
    verify_source_layout_contract()
    payload = bin_path.read_bytes()
    if not payload:
        raise AssertionError("empty raw payload")
    with elf_path.open("rb") as stream:
        elf = ELFFile(stream)
        if elf.elfclass != 32 or not elf.little_endian or elf["e_machine"] != "EM_ARM":
            raise AssertionError("payload must be little-endian ELF32 ARM")
        section = elf.get_section_by_name(".payload")
        if section is None or int(section["sh_addr"]) != PAYLOAD_VA:
            raise AssertionError(".payload address mismatch")
        if section.data() != payload:
            raise AssertionError("raw payload differs from ELF .payload")
        loads = [segment for segment in elf.iter_segments() if segment["p_type"] == "PT_LOAD"]
        if len(loads) != 1:
            raise AssertionError("payload ELF must contain exactly one PT_LOAD")
        load = loads[0]
        if (int(load["p_vaddr"]), int(load["p_filesz"]), int(load["p_memsz"]),
                int(load["p_flags"])) != (PAYLOAD_VA, len(payload), len(payload), 5):
            raise AssertionError("payload PT_LOAD geometry/permissions mismatch")
        if PAYLOAD_VA + len(payload) > NEXT_LOAD_VA:
            raise AssertionError("payload reaches the next original PT_LOAD")
        if not (DIRECT_LOOK_STUB_END <= PAYLOAD_VA):
            raise AssertionError("payload overlaps the runtime direct-look stub")
        for expected_string in EXPECTED_PAYLOAD_STRINGS:
            encoded = expected_string.encode("utf-8") + b"\0"
            if encoded not in payload:
                raise AssertionError(f"missing UTF-8 payload string {expected_string!r}")

        symtab = symbols(elf, ".symtab")
        verify_no_absolute_jump_table(payload, symtab)
        required = (
            "guest_settings_activate_hook",
            "guest_settings_check_dispatch",
            "guest_settings_controls_activate_hook",
            "guest_settings_controls_check_dispatch",
            "guest_settings_options_nav_dispatch",
            "guest_settings_activate_impl",
            "guest_settings_controls_activate_impl",
            "guest_settings_controls_check_impl",
            "guest_settings_options_nav_impl",
            "guest_settings_handle_event",
            "guest_settings_tick",
            "guest_settings_payload_end",
        )
        for name in required:
            if name not in symtab:
                raise AssertionError(f"missing payload symbol {name}")
        if symtab["guest_settings_activate_hook"][0] != PAYLOAD_VA:
            raise AssertionError("activate entry is not first")
        if int(elf.header["e_entry"]) != PAYLOAD_VA:
            raise AssertionError("ELF entry mismatch")
        if symtab["guest_settings_payload_end"][0] != PAYLOAD_VA + len(payload):
            raise AssertionError("payload end symbol mismatch")

        relocations = sum(
            section.num_relocations()
            for section in elf.iter_sections()
            if isinstance(section, RelocationSection)
        )
        if relocations:
            raise AssertionError(f"payload retains {relocations} relocation(s)")
        undefined = sorted(
            name for name, (_, _, shndx) in symtab.items() if shndx == "SHN_UNDEF"
        )
        if undefined:
            raise AssertionError(f"payload retains undefined symbols: {undefined}")

        hook_manifest: dict[str, dict[str, object]] = {}
        for name, source, preimage, symbol_name in HOOK_SPECS:
            patch = arm_branch(source, symtab[symbol_name][0], link=False)
            hook_manifest[name] = {
                "va": source,
                "before": preimage.hex(),
                "after": patch.hex(),
                "disassembly": " ".join(one_instruction(patch, source)),
            }

        md = Cs(CS_ARCH_ARM, CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN)
        entry_disassembly = [
            f"{insn.address:08x}: {insn.mnemonic} {insn.op_str}".rstrip()
            for insn in md.disasm(payload[:320], PAYLOAD_VA)
        ]
        verify_activate_rejoin(payload, symtab)
        verify_navigation_veneers(payload, symtab)
        verify_c_dispatch_semantics(payload, symtab)
        verify_focus_lifetime_semantics(payload, symtab)
        verify_layout_construction(payload, symtab)

    return {
        "elf": str(elf_path.resolve()),
        "binary": str(bin_path.resolve()),
        "sha256": sha256(payload),
        "size": len(payload),
        "start_va": PAYLOAD_VA,
        "end_va": PAYLOAD_VA + len(payload),
        "state_va": STATE_VA,
        "state_end_va": STATE_END_VA,
        "symbols": {name: symtab[name][0] for name in required},
        "hooks": hook_manifest,
        "entry_disassembly": entry_disassembly,
    }


def guest_proof(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    digest = sha256(data)
    if digest not in KNOWN_GUEST_HASHES:
        raise AssertionError(f"unrecognized guest hash for {path}: {digest}")
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        dynsyms = symbols(elf, ".dynsym")
        for name, expected in EXPECTED_DYNSYMS.items():
            actual = dynsyms.get(name)
            if actual is None or actual[0] != expected:
                raise AssertionError(
                    f"{path}: {name} address {actual!r}, expected {expected:#x}"
                )
        dynsym_section = elf.get_section_by_name(".dynsym")
        dynsym_types = {
            symbol.name: symbol["st_info"]["type"]
            for symbol in dynsym_section.iter_symbols()
            if symbol.name
        }
        focus_symbol = (
            "_ZN17rglWindow_manager30set_active_widget_to_highlightEP14rglWidget_base"
        )
        if dynsyms[focus_symbol][1] != 0x228 or \
                dynsym_types.get(focus_symbol) != "STT_FUNC":
            raise AssertionError("native focus setter ABI symbol size/type drift")
        if dynsyms["rgl_window_manager"][1] != 0xEB70 or \
                dynsym_types.get("rgl_window_manager") != "STT_OBJECT":
            raise AssertionError("rgl_window_manager object size/type drift")
        # Dynsym includes the 0x64-byte literal pool after the 0x5074-byte code body.
        if dynsyms["_ZN14Options_window8activateEv"][1] != 0x50D8:
            raise AssertionError("Options_window::activate dynsym size is not 0x50D8")

        for address, expected in EXPECTED_INSTRUCTIONS.items():
            offset = file_offset(elf, address, 4)
            actual = one_instruction(data[offset : offset + 4], address)
            if actual != expected:
                raise AssertionError(
                    f"{path}: {address:#x} disassembles as {actual}, expected {expected}"
                )
        for name, address, preimage, _symbol_name in HOOK_SPECS:
            offset = file_offset(elf, address, len(preimage))
            if data[offset : offset + len(preimage)] != preimage:
                raise AssertionError(f"{name} hook preimage mismatch")
        language_offset = file_offset(elf, LANGUAGE_LITERAL_VA, 4)
        if u32(data, language_offset) != EXPECTED_LANGUAGE_WORD:
            raise AssertionError("guest language literal mismatch")

        if any(data[PAYLOAD_VA : PAYLOAD_VA + 0x2000]):
            raise AssertionError(f"settings payload slot is occupied in {path}")
        if any(data[DIRECT_LOOK_STUB_VA:DIRECT_LOOK_STUB_END]):
            raise AssertionError(f"runtime direct-look stub preimage is occupied in {path}")

        loads = [s for s in elf.iter_segments() if s["p_type"] == "PT_LOAD"]
        if len(loads) < 2:
            raise AssertionError("guest has fewer than two PT_LOAD segments")
        rx, rw = loads[0], loads[1]
        if (int(rx["p_offset"]), int(rx["p_vaddr"]), int(rx["p_flags"])) != (0, 0, 5):
            raise AssertionError("unexpected RX PT_LOAD geometry")
        if int(rw["p_offset"]) != NEXT_LOAD_VA or int(rw["p_vaddr"]) != NEXT_LOAD_VA:
            raise AssertionError("unexpected RW PT_LOAD geometry")

    return {
        "path": str(path.resolve()),
        "sha256": digest,
        "role": KNOWN_GUEST_HASHES[digest],
        "rx_filesz": int(rx["p_filesz"]),
        "rw_memsz": int(rw["p_memsz"]),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--guest", type=Path, action="append", default=[])
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()

    proof = payload_proof(args.elf, args.bin)
    proof["guests"] = [guest_proof(path) for path in args.guest]
    if args.manifest:
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(json.dumps(proof, indent=2) + "\n", encoding="utf-8")
    print(
        f"PASS payload sha256={proof['sha256']} size={proof['size']:#x} "
        f"range={proof['start_va']:#010x}..{proof['end_va']:#010x}"
    )
    for guest in proof["guests"]:
        print(
            f"PASS guest {guest['role']} sha256={guest['sha256']} "
            f"rx_filesz={guest['rx_filesz']:#x} rw_memsz={guest['rw_memsz']:#x}"
        )


if __name__ == "__main__":
    main()
