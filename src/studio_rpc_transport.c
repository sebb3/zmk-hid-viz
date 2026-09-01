#include "command_dispatch.h"
#include "hid_viz_send.h"

#include <zmk/endpoints_types.h>
#include <zmk/event_manager.h>
#include <zmk/studio/rpc.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define STUDIO_RPC_REPORT 0xB0
#define STUDIO_RPC_HEADER_SIZE 2
#define STUDIO_RPC_CHUNK_SIZE (CONFIG_RAW_HID_REPORT_SIZE - STUDIO_RPC_HEADER_SIZE)

static bool rx_active;

static int start_rx(void) {
    rx_active = true;
    return 0;
}

static int stop_rx(void) {
    rx_active = false;
    return 0;
}

static void tx_notify(struct ring_buf *buf, size_t written, bool message_done, void *user_data) {
    ARG_UNUSED(written);
    ARG_UNUSED(user_data);

    if (!message_done && ring_buf_size_get(buf) < STUDIO_RPC_CHUNK_SIZE) {
        return;
    }

    uint8_t report[CONFIG_RAW_HID_REPORT_SIZE];
    while (ring_buf_size_get(buf) > 0) {
        memset(report, 0, sizeof(report));
        report[0] = STUDIO_RPC_REPORT;
        report[1] = ring_buf_get(buf, &report[STUDIO_RPC_HEADER_SIZE], STUDIO_RPC_CHUNK_SIZE);
        hid_viz_send(report, sizeof(report));
    }
}

static int studio_rpc_command_handler(const zmk_event_t *eh) {
    const struct raw_hid_command_event *event = as_raw_hid_command_event(eh);
    if (!rx_active || event == NULL || event->length < STUDIO_RPC_HEADER_SIZE ||
        event->data[0] != STUDIO_RPC_REPORT) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint8_t length = MIN(event->data[1], event->length - STUDIO_RPC_HEADER_SIZE);
    struct ring_buf *buf = zmk_rpc_get_rx_buf();
    uint32_t accepted = 0;
    while (accepted < length) {
        accepted += ring_buf_put(buf, &event->data[STUDIO_RPC_HEADER_SIZE + accepted],
                                 length - accepted);
        zmk_rpc_rx_notify();
        if (accepted < length) {
            k_sleep(K_MSEC(1));
        }
    }

    return ZMK_EV_EVENT_HANDLED;
}

ZMK_LISTENER(hid_viz_studio_rpc, studio_rpc_command_handler);
ZMK_SUBSCRIPTION(hid_viz_studio_rpc, raw_hid_command_event);

ZMK_RPC_TRANSPORT(hid_viz_usb, ZMK_TRANSPORT_USB, start_rx, stop_rx, NULL, tx_notify);
ZMK_RPC_TRANSPORT(hid_viz_ble, ZMK_TRANSPORT_BLE, start_rx, stop_rx, NULL, tx_notify);
