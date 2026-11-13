#pragma once

#include <stdint.h>

#include <zephyr/sys/util.h>               /* BUILD_ASSERT, _CONCAT, MIN */
#include <zephyr/sys/iterable_sections.h>  /* STRUCT_SECTION_ITERABLE / _FOREACH / _COUNT */

/*
 * Capability registry — single source of truth for the wire constants and the
 * capability descriptor used by the manifest.
 *
 * v2 (self-registration): every file that implements a capability registers it
 * with HID_VIZ_CAP_REGISTER(), which drops a `struct hid_viz_cap` into a ROM
 * iterable linker section (see src/hid_viz_cap.ld). manifest.c iterates that
 * section instead of consulting a central table, so declaring a capability and
 * advertising it are the same line of code, living next to the implementation
 * (its Kconfig gating travels with it). The legacy notifier.c / command_handler.c
 * now include this header too — their pre-existing local #defines carry identical
 * values, so redefinition is harmless.
 */

/* ============================================================
 * Roles — what a device does with a capability
 * ============================================================ */
#define ROLE_IDENTITY    0   /* Not a capability; identity/discovery metadata */
#define ROLE_NOTIFIES    1   /* Device emits this event to the host */
#define ROLE_HANDLES     2   /* Device accepts this command from the host */
#define ROLE_TRIGGERS    3   /* Device originates this action (fire-and-forget) */

/* ============================================================
 * Tiers — availability / stability of the capability
 * ============================================================ */
#define TIER_CORE        0   /* Always present when the module is enabled */
#define TIER_FW_SPECIFIC 1   /* Present only if the firmware implements it */
#define TIER_PROFILE     2   /* Optional profile (RGB, pointing, etc.) */
#define TIER_OPTIONAL    3   /* Opt-in; may be absent even when module is on */

/* ============================================================
 * Identity sub-types (carried in the tier field when role == ROLE_IDENTITY)
 * ============================================================ */
#define ID_TIER_NAME    0   /* Board name   (CONFIG_ZMK_KEYBOARD_NAME) */
#define ID_TIER_CONFIG  1   /* Config ID    (CONFIG_HID_VIZ_CONFIG_ID) */

/* ============================================================
 * Wire type bytes — outbound notifications (keyboard -> host)
 * ============================================================ */
#define MSG_LAYER_STATE      0xFF   /* Active layer bitmask */
#define MSG_KEY_EVENT        0xF1   /* Key press/release */

/* ============================================================
 * Wire type bytes — existing inbound commands / responses
 * ============================================================ */
#define CMD_GET_DEVICE_INFO  0xFD
#define RSP_DEVICE_INFO      0xFE
#define CMD_GET_CONFIG_ID    0xFB
#define RSP_CONFIG_ID        0xFA
#define CMD_SET_LAYER        0xFC   /* core.layer.set (full bitmask) */

/* ============================================================
 * Wire type bytes — build identity (CONFIG_HID_VIZ_COMMANDS)
 *
 * The host build stamps the firmware image with the source commit it was
 * built from (CONFIG_HID_VIZ_BUILD_COMMIT, set by the host build system,
 * e.g. ZMK Live's Nix build from the flake's shortRev). The host queries it
 * with CMD_GET_BUILD_COMMIT; the central half answers from its own image
 * with RSP_BUILD_COMMIT. Raw HID exists only on the central half, so the
 * peripheral half is never queried directly: both halves are built from the
 * same source commit in one build, so the central half's commit identifies
 * the pair.
 * ============================================================ */
#define CMD_GET_BUILD_COMMIT 0xF6   /* firmware build-commit query */
#define RSP_BUILD_COMMIT     0xF5   /* firmware build-commit response */

/* ============================================================
 * Wire type bytes — RGB profile (M5, CONFIG_HID_VIZ_RGB)
 * 0xD2 (core.rgb.setKey) is reserved for QMK RGB Matrix devices and is
 * never implemented or advertised by ZMK — per-key colour on ZMK is
 * locally derived from layer state (drive it via core.layer.*).
 * ============================================================ */
#define MSG_RGB_CHANGED      0xD0   /* core.rgb.changed { on, h, s, v, effect } */
#define CMD_RGB_SET          0xD1   /* core.rgb.set     { on?, h?, s?, v?, effect? } */

/* ============================================================
 * Wire type bytes — signal.* namespace (CONFIG_HID_VIZ_SIGNAL)
 *
 * Opaque host-defined triggers: the keyboard fires an id, all meaning lives in
 * the listening host's config. Per-id fixed nodes advertise as "signal.fire/<id>"
 * so a host scan can enumerate exactly which ids the board fires; the flexible
 * 1-cell &signal form advertises the bare "signal.fire". 0xC1/0xC2 are reserved
 * for a future signal.value / signal.delta (analog / relative input).
 * ============================================================ */
#define SIGNAL_FIRE          0xC0   /* signal.fire { id:uint8 } — fire-and-forget */

/* ============================================================
 * Wire type bytes — M1 additions
 * ============================================================ */
#define CMD_GET_MANIFEST     0xF9   /* Request manifest stream */
#define RSP_MANIFEST_ENTRY   0xF8   /* One manifest entry per report */
#define CMD_LAYER_SET_BASE   0xF4   /* core.layer.setBase   { index } */
#define CMD_LAYER_ACTIVATE   0xF3   /* core.layer.activate  { ref[2], index } */
#define CMD_LAYER_DEACTIVATE 0xF2   /* core.layer.deactivate{ ref[2], index } */
#define RSP_CONFIRM          0xF7   /* Action confirmation  { ref[2], ok } */

/*
 * RSP_MANIFEST_ENTRY report layout
 *   byte[0]   = RSP_MANIFEST_ENTRY (0xF8)
 *   byte[1]   = sequence index (uint8, 0-based logical entry index)
 *   byte[2]   = flags
 *                 bit0 = MANIFEST_FLAG_LAST      (last logical entry in the stream)
 *                 bit1 = MANIFEST_FLAG_CONTINUES (string continues in next physical report)
 *   byte[3]   = role    (ROLE_*;  0 on continuation reports)
 *   byte[4]   = tier    (TIER_* or ID_TIER_*;  0 on continuation reports)
 *   byte[5]   = confirm (0/1;  0 on continuation reports)
 *   byte[6..] = string chunk (null-terminated on the final chunk only)
 *
 * Strings longer than one report (e.g. a 36-char GUID config ID) are split across
 * several physical reports that share the same sequence index. All but the last
 * carry MANIFEST_FLAG_CONTINUES; the host concatenates chunks until it is clear.
 */
#define MANIFEST_ENTRY_HEADER_SIZE  6
#define MANIFEST_FLAG_LAST          0x01
#define MANIFEST_FLAG_CONTINUES     0x02
/* Usable bytes for string data in one physical report */
#define MANIFEST_ENTRY_CHUNK_SIZE   (CONFIG_RAW_HID_REPORT_SIZE - MANIFEST_ENTRY_HEADER_SIZE)

/* ============================================================
 * Capability descriptor — one row per capability in the manifest table
 * ============================================================ */
struct hid_viz_cap {
    uint8_t      type;      /* Wire byte (a CMD_ or MSG_ constant) */
    uint8_t      role;      /* ROLE_* */
    uint8_t      tier;      /* TIER_* */
    uint8_t      confirm;   /* 0 or 1 */
    const char  *id;        /* Null-terminated capability ID string */
};

/* ============================================================
 * Self-registration into the ROM iterable section `hid_viz_cap`.
 *
 * Usage (file scope, in the file that implements the capability):
 *   HID_VIZ_CAP_REGISTER(cap_core_layer_set, CMD_SET_LAYER,
 *                        ROLE_HANDLES, TIER_CORE, 0, "core.layer.set");
 *
 * `_name` must be unique within its translation unit (it names the static
 * section variable). IDs longer than one report (e.g. the longer pointing
 * trigger IDs) are fine — send_manifest_entry() splits them across reports like
 * any other string; the BUILD_ASSERT only catches a grossly malformed ID. The
 * entry is `static` + `__used`, so the linker keeps it even though nothing
 * references it by symbol — manifest.c reaches it via the section.
 * ============================================================ */
#define HID_VIZ_CAP_ID_MAX_SIZE 64   /* incl. null; longer is a bug, not a long ID */

#define HID_VIZ_CAP_REGISTER(_name, _type, _role, _tier, _confirm, _id)        \
    BUILD_ASSERT(sizeof(_id) <= HID_VIZ_CAP_ID_MAX_SIZE,                        \
                 "Capability ID unreasonably long: " _id);                     \
    static STRUCT_SECTION_ITERABLE(hid_viz_cap, _name) = {                      \
        .type = (_type), .role = (_role), .tier = (_tier),                     \
        .confirm = (_confirm), .id = (_id),                                    \
    }

/* ============================================================
 * Emit action table — maps the `action` string-enum index of the
 * zmk,behavior-hid-viz-emit-fixed binding to its wire byte, tier, and ID.
 *
 * The index ORDER here MUST match the `enum:` list in
 * dts/bindings/behaviors/zmk,behavior-hid-viz-emit-fixed.yaml. The wire bytes
 * mirror <dt-bindings/hid_viz/emit.h> (the 2-cell &hid_viz_emit form's params).
 *
 * The HID_VIZ_ACTION_* macros are integer/string literals, so ACTION_* below
 * resolve at compile time and are usable in static initializers (config structs
 * and the HID_VIZ_CAP_REGISTER trigger entries).
 * ============================================================ */
#define HID_VIZ_ACTION_0_BYTE  0xE1
#define HID_VIZ_ACTION_0_TIER  TIER_CORE
#define HID_VIZ_ACTION_0_ID    "core.pointing.dpi.set"

#define HID_VIZ_ACTION_1_BYTE  0xE2
#define HID_VIZ_ACTION_1_TIER  TIER_CORE
#define HID_VIZ_ACTION_1_ID    "core.pointing.dpi.setIndex"

#define HID_VIZ_ACTION_2_BYTE  0xE9
#define HID_VIZ_ACTION_2_TIER  TIER_FW_SPECIFIC
#define HID_VIZ_ACTION_2_ID    "core.pointing.dragScroll.set"

#define HID_VIZ_ACTION_3_BYTE  0xEB
#define HID_VIZ_ACTION_3_TIER  TIER_FW_SPECIFIC
#define HID_VIZ_ACTION_3_ID    "core.pointing.snipe.set"

/* idx -> wire byte / tier / id string (idx = DT_INST_ENUM_IDX(n, action)).
 * Double-indirection via _CONCAT expands `idx` before pasting. */
#define HID_VIZ_ACTION_BYTE(idx)  _CONCAT(_CONCAT(HID_VIZ_ACTION_, idx), _BYTE)
#define HID_VIZ_ACTION_TIER(idx)  _CONCAT(_CONCAT(HID_VIZ_ACTION_, idx), _TIER)
#define HID_VIZ_ACTION_ID(idx)    _CONCAT(_CONCAT(HID_VIZ_ACTION_, idx), _ID)
