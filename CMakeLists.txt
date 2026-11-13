if(CONFIG_HID_VIZ)
    if(NOT CONFIG_RAW_HID)
        message(FATAL_ERROR "zmk-hid-viz requires CONFIG_RAW_HID=y. Add zmk-raw-hid to your west projects list and enable CONFIG_RAW_HID=y in your .conf.")
    endif()

    # ROM iterable section that collects HID_VIZ_CAP_REGISTER() entries from
    # whichever source files below get compiled in. Always present (harmless when
    # empty); manifest.c walks it. See src/hid_viz_cap.ld.
    zephyr_linker_sources(SECTIONS src/hid_viz_cap.ld)

    # Serialized transmit funnel — every sender below routes through hid_viz_send()
    # so the manifest worker thread and the inbound command handlers can't both be
    # inside the (non-thread-safe) transport send_report() at once.
    target_sources(app PRIVATE src/hid_viz_send.c)

    # Shared inbound-command dispatcher — the single listener on
    # raw_hid_received_event. It defers every command onto a worker thread (via
    # raw_hid_command_event) so no handler runs on the USB SetReport callback
    # context. Always compiled: it is the one entry point all command handlers
    # below depend on. See src/command_dispatch.h.
    target_sources(app PRIVATE src/command_dispatch.c)

    if(CONFIG_HID_VIZ_LAYER_EVENTS OR CONFIG_HID_VIZ_KEY_EVENTS)
        target_sources(app PRIVATE src/notifier.c)
    endif()

    if(CONFIG_HID_VIZ_COMMANDS)
        target_sources(app PRIVATE src/command_handler.c)
        target_sources(app PRIVATE src/layer_actions.c)
        target_sources(app PRIVATE src/bootloader.c)
    endif()

    if(CONFIG_HID_VIZ_RGB)
        target_sources(app PRIVATE src/rgb_actions.c)
    endif()

    if(CONFIG_HID_VIZ_MANIFEST)
        target_sources(app PRIVATE src/manifest.c)
    endif()

    if(CONFIG_HID_VIZ_EMIT)
        target_sources(app PRIVATE src/emit_send.c)
        target_sources(app PRIVATE src/behavior_hid_viz_emit.c)
        target_sources(app PRIVATE src/behavior_hid_viz_emit_fixed.c)
        # Expose include/dt-bindings/hid_viz/emit.h to keymap preprocessing.
        zephyr_include_directories(include)
    endif()

    if(CONFIG_HID_VIZ_SIGNAL)
        target_sources(app PRIVATE src/signal_send.c)
        target_sources(app PRIVATE src/behavior_hid_signal.c)
        target_sources(app PRIVATE src/behavior_hid_signal_fixed.c)
        # No include dir needed: ids are raw numbers / an `id` devicetree property.
    endif()

    if(CONFIG_HID_VIZ_STUDIO)
        if(CONFIG_ZMK_STUDIO_TRANSPORT_UART)
            message(FATAL_ERROR "HID_VIZ_STUDIO owns the USB Studio endpoint. Disable ZMK_STUDIO_TRANSPORT_UART.")
        endif()
        target_sources(app PRIVATE src/studio_rpc_transport.c)
    endif()
endif()
