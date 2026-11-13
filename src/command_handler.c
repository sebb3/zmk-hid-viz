#include "command_dispatch.h"

#include <zmk/event_manager.h>
#include <zmk/keymap.h>

#include <string.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "capabilities.h"
#include "hid_viz_send.h"

extern void send_layer_state(void);

/* Manifest self-registration. The local #defines below mirror capabilities.h's
 * values exactly, so the redefinitions are harmless. */
HID_VIZ_CAP_REGISTER(cap_core_layer_set, CMD_SET_LAYER, ROLE_HANDLES, TIER_CORE, 0,
                     "core.layer.set");
/*
 * Inbound command handler: receives commands from the host app over Raw HID
 * and responds or acts on them.
 *
 * Command protocol (host -> keyboard):
 *   byte[0] = command type
 *   byte[1..] = command-specific data
 *
 * Commands:
 *   0xFD - Get device info (responds with board ID and version)
 *   0xFC - Set layer state (bytes[1-4] = uint32 bitmask of layers to activate, LE)
 *   0xFB - Get config ID (responds with layout/config identifier string)
 *   0xF6 - Get build commit (responds with the baked source commit string)
 *
 * Responses (keyboard -> host):
 *   0xFE - Device info response
 *   0xFA - Config ID response
 *   0xF5 - Build commit response
 */

#define CMD_GET_DEVICE_INFO  0xFD
#define CMD_SET_LAYER        0xFC
#define CMD_GET_CONFIG_ID    0xFB
#define CMD_GET_BUILD_COMMIT 0xF6
#define RSP_DEVICE_INFO      0xFE
#define RSP_CONFIG_ID        0xFA
#define RSP_BUILD_COMMIT     0xF5

#define HID_VIZ_PROTOCOL_VERSION 0x01

static uint8_t response_buf[CONFIG_RAW_HID_REPORT_SIZE];

static void send_device_info(void) {
    memset(response_buf, 0, sizeof(response_buf));
    response_buf[0] = RSP_DEVICE_INFO;
    response_buf[1] = HID_VIZ_PROTOCOL_VERSION;

    const char *board_id = CONFIG_ZMK_KEYBOARD_NAME;
    size_t id_len = strlen(board_id);
    if (id_len > CONFIG_RAW_HID_REPORT_SIZE - 2) {
        id_len = CONFIG_RAW_HID_REPORT_SIZE - 2;
    }
    memcpy(&response_buf[2], board_id, id_len);

    hid_viz_send(response_buf, sizeof(response_buf));
}

static void send_config_id(void) {
    memset(response_buf, 0, sizeof(response_buf));
    response_buf[0] = RSP_CONFIG_ID;

    const char *config_id = CONFIG_HID_VIZ_CONFIG_ID;
    size_t id_len = strlen(config_id);
    if (id_len > CONFIG_RAW_HID_REPORT_SIZE - 1) {
        id_len = CONFIG_RAW_HID_REPORT_SIZE - 1;
    }
    memcpy(&response_buf[1], config_id, id_len);

    hid_viz_send(response_buf, sizeof(response_buf));
}

static void send_build_commit(void) {
    memset(response_buf, 0, sizeof(response_buf));
    response_buf[0] = RSP_BUILD_COMMIT;

    const char *commit = CONFIG_HID_VIZ_BUILD_COMMIT;
    size_t commit_len = strlen(commit);
    if (commit_len > CONFIG_RAW_HID_REPORT_SIZE - 1) {
        commit_len = CONFIG_RAW_HID_REPORT_SIZE - 1;
    }
    memcpy(&response_buf[1], commit, commit_len);

    hid_viz_send(response_buf, sizeof(response_buf));
}

static void handle_set_layer_state(uint32_t layer_state) {
    LOG_INF("Command: set layer state 0x%08x", layer_state);

    /* Suppress intermediate layer-state notifications during the
     * deactivate/activate cycle. Without this, each zmk_keymap_layer_deactivate
     * triggers a zmk_layer_state_changed event, which the notifier sends as
     * an IN report — flooding the host with intermediate states (typically
     * 0x01 = just layer 0) before the one clean final state below. (The handler
     * itself now runs off the USB SetReport callback on the dispatch worker, so
     * these sends no longer threaten the USB peripheral; suppression is purely
     * to avoid the intermediate-report flood.) */
    hid_viz_suppress_notifications = true;

    for (uint8_t i = 1; i < 32; i++) {
        if (zmk_keymap_layer_active(i)) {
            zmk_keymap_layer_deactivate(i);
        }
    }

    for (uint8_t i = 1; i < 32; i++) {
        if (layer_state & BIT(i)) {
            zmk_keymap_layer_activate(i);
        }
    }

    /* Re-enable notifications and send one clean final state */
    hid_viz_suppress_notifications = false;
    send_layer_state();
}

static int command_handler(const zmk_event_t *eh) {
    struct raw_hid_command_event *event = as_raw_hid_command_event(eh);
    if (event == NULL || event->length == 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint8_t cmd = event->data[0];

    switch (cmd) {
    case CMD_GET_DEVICE_INFO:
        LOG_INF("Command: get device info");
        send_device_info();
        break;

    case CMD_GET_CONFIG_ID:
        LOG_INF("Command: get config ID");
        send_config_id();
        break;

    case CMD_GET_BUILD_COMMIT:
        LOG_INF("Command: get build commit");
        send_build_commit();
        break;

    case CMD_SET_LAYER:
        if (event->length >= 5) {
            uint32_t state;
            memcpy(&state, &event->data[1], sizeof(uint32_t));
            handle_set_layer_state(state);
        }
        break;

    default:
        /* Unknown command — let other listeners handle it */
        LOG_DBG("Unknown command: 0x%02x", cmd);
        break;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(hid_viz_command_handler, command_handler);
ZMK_SUBSCRIPTION(hid_viz_command_handler, raw_hid_command_event);
