#include "command_dispatch.h"

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/usb.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
#include <zmk/events/position_state_changed.h>
#endif

#include <string.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "capabilities.h"

/*
 * Restart-into-bootloader command (host -> keyboard).
 *
 * A third listener on raw_hid_command_event (the dispatcher's off-callback
 * re-raise; see command_dispatch.h), alongside command_handler.c and
 * layer_actions.c; ZMK allows multiple listeners per event and each one
 * bubbles, so the three never conflict (their command bytes are disjoint).
 *
 * Lets the host restart either half of a split keyboard into its UF2
 * bootloader without a key combination, for firmware updates:
 *
 *   0xF0  core.bootloader.restart  { byte[1] = target, bytes[2..5] = magic }
 *
 *   target 0x00: this half (the central, which receives the Raw HID report).
 *                 Runs the same `bootload` behavior node the `&bootloader`
 *                 keymap behavior uses, so the reboot path (Adafruit UF2
 *                 GPREGRET magic + sys_reboot) is identical to pressing the key.
 *   target 0x01: the split peripheral (right half), relayed over split from
 *                 the central with zmk_split_central_invoke_behavior() — the
 *                 same mechanism global-locality behaviors use to run on
 *                 peripherals. The peripheral looks up its own local `bootload`
 *                 node and reboots itself; no new split message was needed.
 *
 * RISK — READ BEFORE SENDING: this reboots the keyboard with no confirmation
 * and no reply (a rebooting half answers nothing). Protections, in order:
 *   1. The report must carry the 4-byte magic below; anything else is ignored.
 *      The magic is an accident guard, not a secret — it is public in this
 *      source and in the host. Any software that can open the Raw HID device
 *      can reboot the keyboard.
 *   2. The central half must be USB-powered (zmk_usb_is_powered()); otherwise
 *      the command is ignored. A stray command received over BLE-only power
 *      therefore never reboots anything.
 *   3. Peripheral-side USB power cannot be checked from the central — the
 *      split protocol carries no USB state — so target 0x01 additionally relies
 *      on the host's update flow, which keeps USB cables on both halves (the
 *      UF2 drive the firmware is copied to only exists over USB anyway).
 * Host rule: restart the right half FIRST. The left (central) half carries the
 * Raw HID signal; once it reboots, nothing can reach the right half anymore.
 *
 * Support is advertised in the manifest stream (CMD_GET_MANIFEST 0xF9) as the
 * `core.bootloader.restart` capability, so the host knows when to fall back
 * to the manual key-combination instructions.
 */

#define BOOTLOADER_TARGET_LEFT 0x00
#define BOOTLOADER_TARGET_RIGHT 0x01

/* Fixed accident-guard magic, bytes[2..5] — ASCII "ZMK!". */
static const uint8_t bootloader_magic[4] = {0x5A, 0x4D, 0x4B, 0x21};

/* Manifest self-registration — the restart handler this file implements. */
HID_VIZ_CAP_REGISTER(cap_core_bootloader_restart, CMD_RESTART_BOOTLOADER, ROLE_HANDLES,
                     TIER_CORE, 0, "core.bootloader.restart");

/* The `bootload` behavior node — the same one `&bootloader` presses. Its node
 * name (not the `bootloader` label the keymap uses) is what behavior_dev
 * carries and what the split invoke path allows (<= 8 chars + NUL). */
static const struct zmk_behavior_binding bootloader_binding = {
    .behavior_dev = "bootload",
    .param1 = 0,
    .param2 = 0,
};

static struct zmk_behavior_binding_event bootloader_event(void) {
    return (struct zmk_behavior_binding_event){
        .position = 0,
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };
}

static void handle_restart_left(void) {
    LOG_INF("Command: restart this half into bootloader");
    int err = zmk_behavior_invoke_binding(&bootloader_binding, bootloader_event(), true);
    if (err != 0) {
        /* EVENT_SOURCE locality + LOCAL source invokes locally; anything else
         * means the behavior node is missing from this build. */
        LOG_ERR("Failed to invoke local bootloader behavior: %d", err);
    }
}

static void handle_restart_right(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    LOG_INF("Command: restart peripheral into bootloader via split");
    int err = zmk_split_central_invoke_behavior(0, (struct zmk_behavior_binding *)&bootloader_binding,
                                                bootloader_event(), true);
    if (err != 0) {
        /* -ENODEV when no split transport is up: the right half is unreachable,
         * so there is nothing to reboot. */
        LOG_WRN("Failed to relay bootloader restart to peripheral: %d", err);
    }
#else
    LOG_WRN("Bootloader restart for right half ignored: not a split central");
#endif
}

static int bootloader_handler(const zmk_event_t *eh) {
    struct raw_hid_command_event *event = as_raw_hid_command_event(eh);
    if (event == NULL || event->length == 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (event->data[0] != CMD_RESTART_BOOTLOADER) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (event->length < 6) {
        LOG_WRN("Bootloader restart ignored: %d bytes, need 6", event->length);
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (memcmp(&event->data[2], bootloader_magic, sizeof(bootloader_magic)) != 0) {
        LOG_WRN("Bootloader restart ignored: bad magic");
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Guard 2 (see RISK above): only ever reboot a USB-powered half. */
    if (!zmk_usb_is_powered()) {
        LOG_WRN("Bootloader restart ignored: USB not powered");
        return ZMK_EV_EVENT_BUBBLE;
    }

    switch (event->data[1]) {
    case BOOTLOADER_TARGET_LEFT:
        handle_restart_left();
        break;
    case BOOTLOADER_TARGET_RIGHT:
        handle_restart_right();
        break;
    default:
        LOG_WRN("Bootloader restart ignored: unknown target 0x%02x", event->data[1]);
        break;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(hid_viz_bootloader, bootloader_handler);
ZMK_SUBSCRIPTION(hid_viz_bootloader, raw_hid_command_event);
