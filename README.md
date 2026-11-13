# zmk-hid-viz

A ZMK firmware module that sends real-time layer-state and key-event notifications to a host visualization app, and accepts commands from the host. Runs on top of the [zmk-raw-hid](https://github.com/zzeneg/zmk-raw-hid) transport module.

## What it does

- **Layer notifications**: Every layer change (`&mo`, `&lt`, `&tog`, `&to`, combos, tap-dance — everything) is reported to the host in real time.
- **Key event notifications**: Every physical key press and release is reported with its matrix position.
- **Command handling**: The host app can query device info and control the active layer state.

Transport (USB + BLE) is handled entirely by zmk-raw-hid; this module only consumes its `raw_hid_received_event` / `raw_hid_sent_event` events.

## Installation

Add both modules to your `config/west.yml`:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: zzeneg
      url-base: https://github.com/zzeneg
    - name: ovandongen
      url-base: https://github.com/ovandongen
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: zmk-raw-hid
      remote: zzeneg
      revision: main
    - name: zmk-hid-viz
      remote: ovandongen
      revision: main
  self:
    path: config
```

Add to your `.conf` file:

```
CONFIG_RAW_HID=y
CONFIG_HID_VIZ=y
```

Both lines are required — `CONFIG_RAW_HID` enables the transport (provided by `zmk-raw-hid`) and `CONFIG_HID_VIZ` enables the messaging layer on top of it.

Add the `raw_hid_adapter` shield (provided by zmk-raw-hid) to your left/central half in `build.yaml`:

```yaml
include:
  - board: your_board_lh
    shield: raw_hid_adapter
  - board: your_board_rh
```

## Configuration

All options in your `.conf` file:

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_HID_VIZ` | n | Enable the module |
| `CONFIG_HID_VIZ_LAYER_EVENTS` | y | Send layer state notifications |
| `CONFIG_HID_VIZ_KEY_EVENTS` | y | Send key press/release events |
| `CONFIG_HID_VIZ_COMMANDS` | y | Accept commands from host app |
| `CONFIG_HID_VIZ_RGB` | n | RGB profile: handle `0xD1` set commands, emit `0xD0` change receipts (requires `CONFIG_ZMK_RGB_UNDERGLOW=y`) |
| `CONFIG_HID_VIZ_SIGNAL` | n | Signal behaviors: fire opaque host-defined `0xC0` triggers on key press |
| `CONFIG_HID_VIZ_CONFIG_ID` | "" | Layout/config identifier returned by the `0xFB` query |

Raw HID transport options (`CONFIG_RAW_HID_USAGE_PAGE`, `CONFIG_RAW_HID_USAGE`, `CONFIG_RAW_HID_REPORT_SIZE`, `CONFIG_RAW_HID_DEVICE`) are owned by zmk-raw-hid — see its README for defaults and tuning.

## Message Protocol

### Outbound (keyboard → host)

**Layer state** (byte[0] = `0xFF`):
| Byte | Value | Description |
|------|-------|-------------|
| 0 | `0xFF` | Message type |
| 1 | `0x04` | Width of each bitmask field (bytes) |
| 2-5 | uint32 (LE) | Default layer bitmask (layers 0-31) |
| 6-9 | uint32 (LE) | Active layer bitmask (layers 0-31) |

**Key event** (byte[0] = `0xF1`):
| Byte | Value | Description |
|------|-------|-------------|
| 0 | `0xF1` | Message type |
| 2 | uint8 | Key matrix position |
| 3 | 0/1 | 0 = released, 1 = pressed |

**Device info response** (byte[0] = `0xFE`):
| Byte | Value | Description |
|------|-------|-------------|
| 0 | `0xFE` | Message type |
| 1 | uint8 | Protocol version |
| 2+ | string | Board/keyboard name (null-terminated) |

**Config ID response** (byte[0] = `0xFA`):
| Byte | Value | Description |
|------|-------|-------------|
| 0 | `0xFA` | Message type |
| 1+ | string | Layout/config ID from `CONFIG_HID_VIZ_CONFIG_ID` (null-terminated, empty if unset) |

**RGB changed** (byte[0] = `0xD0`, requires `CONFIG_HID_VIZ_RGB`):
| Byte | Value | Description |
|------|-------|-------------|
| 0 | `0xD0` | Message type (`core.rgb.changed`) |
| 1 | 0/1 | Underglow on/off |
| 2-3 | uint16 (LE) | Hue (0-359) |
| 4 | uint8 | Saturation (0-100) |
| 5 | uint8 | Brightness (0-100) |
| 6 | uint8 | Effect index |

Sent as the receipt after every `0xD1` set. Stock ZMK has no HSB/effect getter,
so h/s/v/effect reflect the last value set *through this module*; changes made
by `&rgb_ug` keymap behaviours bypass it and desync the report (on/off is
always live). When the effect index selects a per-key/layer mode (fork
firmware), the reported hue/saturation are dormant — treat them as advisory.

### Inbound (host → keyboard)

**Get device info** (byte[0] = `0xFD`): Keyboard responds with `0xFE` device info message.

**Get config ID** (byte[0] = `0xFB`): Keyboard responds with `0xFA` config ID message. Host apps use this to auto-load the matching layout definition; an empty string means manual layout selection.

**Set layer state** (byte[0] = `0xFC`):
| Byte | Value | Description |
|------|-------|-------------|
| 0 | `0xFC` | Command type |
| 1-4 | uint32 (LE) | Layer state bitmask (bit N = layer N active, for layers 0-31) |

Layer 0 is always active regardless of the bitmask. The keyboard deactivates all
other layers and then activates every layer whose bit is set, allowing the host
to restore a complete layer stack rather than a single layer.

**Restart into bootloader** (byte[0] = `0xF0`, requires `CONFIG_HID_VIZ_COMMANDS`):
| Byte | Value | Description |
|------|-------|-------------|
| 0 | `0xF0` | Command type (`core.bootloader.restart`) |
| 1 | target | `0x00` = this (central) half, `0x01` = right half via split relay |
| 2-5 | `5A 4D 4B 21` | Fixed magic (`ZMK!`); any other value is ignored |

Restarts the target half into its UF2 bootloader with no key combination, so
a firmware update needs no manual step per half. Target `0x00` runs the same
`bootload` behavior node the `&bootloader` keymap behavior uses (Adafruit UF2
GPREGRET magic + reboot). Target `0x01` relays over split from the central
with the same invoke-behavior mechanism global-locality behaviors use, and the
peripheral reboots itself. There is no reply — a rebooting half answers
nothing. The host restarts the right half first: the left (central) half
carries the Raw HID signal, so once it reboots nothing can reach the right.

Risk: this reboots the keyboard on request, so treat the command with care.
The magic is an accident guard, not a secret — it is public in this source,
and any software able to open the Raw HID device can reboot the keyboard.
The central half additionally ignores the command unless it is USB-powered,
so a stray command received over BLE-only power never reboots anything;
peripheral-side USB power cannot be checked over split (the split protocol
carries no USB state), so the update flow keeps USB cables on both halves.
Support is advertised in the manifest stream as `core.bootloader.restart`;
hosts without it fall back to the manual key combination.

**RGB set** (byte[0] = `0xD1`, requires `CONFIG_HID_VIZ_RGB`):
| Byte | Value | Description |
|------|-------|-------------|
| 0 | `0xD1` | Command type (`core.rgb.set`) |
| 1 | bitmask | Fields present: bit0=on, bit1=h, bit2=s, bit3=v, bit4=effect |
| 2 | 0/1 | Underglow on/off |
| 3-4 | uint16 (LE) | Hue (0-359) |
| 5 | uint8 | Saturation (0-100) |
| 6 | uint8 | Brightness (0-100) |
| 7 | uint8 | Effect index |

Absolute and idempotent. Byte positions are fixed; bytes whose mask bit is
clear are ignored and the corresponding setting keeps its current value.
Colour/effect are applied before on/off, so `{on=1, h, s, v}` switches on
showing the new colour. The keyboard always replies with a `0xD0` changed
receipt — sending an empty mask (`0x00`) applies nothing and therefore acts
as a state query. Per-key colour is deliberately not exposed: on ZMK the
per-key picture derives locally from layer state, so hosts change it with the
layer commands above (`0xD2` / `core.rgb.setKey` is reserved for QMK RGB
Matrix devices).

## Emit behavior (originator)

The optional `&hid_viz_emit` behavior lets a key emit a capability action onto the
bus, fire-and-forget — e.g. a keyboard telling a trackball to change its DPI. The
keyboard never waits for or learns the outcome; it observes the resulting
`*.changed` event if it cares. With no fulfiller present, the emit is a silent
no-op.

Enable it in your `.conf`:

```
CONFIG_HID_VIZ_EMIT=y
```

Add the behavior node to your keymap and bind it (the action constants come from
the module's `dt-bindings` header):

```dts
#include <dt-bindings/hid_viz/emit.h>

/ {
    behaviors {
        hid_viz_emit: hid_viz_emit {
            compatible = "zmk,behavior-hid-viz-emit";
            #binding-cells = <2>;
        };
    };
};
```

Then use it in any layer. First parameter = action, second = a uint32 value:

```dts
&hid_viz_emit POINTING_DPI_SET 800        // -> 0xE1, value 800 (LE)
&hid_viz_emit POINTING_DRAGSCROLL_SET 1   // -> 0xE9, value 1
```

Available action constants: `POINTING_DPI_SET` (0xE1), `POINTING_DPI_SETINDEX`
(0xE2), `POINTING_DRAGSCROLL_SET` (0xE9), `POINTING_SNIPE_SET` (0xEB). The wire
message is `[action, value_uint32_le]` — the same type byte the fulfilling
device's handler consumes. This device lists each emitted action under
`triggers`; routing an emit to a handler is the host hub's job.

### Fixed form (devicetree-configured, editor-friendly)

The 2-cell form above needs a `#include` and two keymap parameters on the key,
which the **MoErgo Glove80 Layout Editor** can't place. For that workflow there's
a **zero-cell** sibling, `zmk,behavior-hid-viz-emit-fixed`: the action and value
live on the node as devicetree properties, so you paste one node into the editor's
*custom behaviors / custom devicetree* box and bind it by label — no include, no
parameters on the key.

```dts
/ {
    behaviors {
        dpi_800: dpi_800 {
            compatible = "zmk,behavior-hid-viz-emit-fixed";
            #binding-cells = <0>;
            action = "core.pointing.dpi.set";   // string enum (capability ID)
            value  = <800>;                       // uint32, sent little-endian
        };
    };
};
```

Then bind `&dpi_800` on any key. `action` is one of the capability ID strings
`"core.pointing.dpi.set"`, `"core.pointing.dpi.setIndex"`,
`"core.pointing.dragScroll.set"`, `"core.pointing.snipe.set"` (mapped to the same
wire bytes as the constants above; an unknown string fails the devicetree build).
Both forms share `CONFIG_HID_VIZ_EMIT` and the same `[action, value_uint32_le]`
wire message. Each fixed node also self-registers its action under `triggers` in
the manifest automatically — declare the node and the device advertises it.

## Signal behavior (generic host triggers)

The optional `&signal` behavior fires an **opaque host-defined id** onto the bus on
key press, fire-and-forget. Unlike emit (which references a *device* capability like
DPI), a signal means nothing on the wire — it just says "id N happened." The
listening host maps the id to whatever it likes (launch an app, run a script, focus
a window). New actions are pure host config; no reflash. See
[docs/signal-capability-spec.md](docs/signal-capability-spec.md) for the full design.

Wire message (byte[0] = `0xC0`):
| Byte | Value | Description |
|------|-------|-------------|
| 0 | `0xC0` | Message type (`signal.fire`) |
| 1 | uint8 | Opaque id (meaning is host config) |

Enable it in your `.conf`:

```
CONFIG_HID_VIZ_SIGNAL=y
```

**1-cell form** — flexible, one parameter is the id:

```dts
/ {
    behaviors {
        signal: signal {
            compatible = "zmk,behavior-hid-signal";
            #binding-cells = <1>;
        };
    };
};
```

```dts
&signal 7      // -> 0xC0, id 7   ("build")
&signal 12     // -> 0xC0, id 12  ("open browser")
```

Put signal keys on a dedicated layer so they don't also type. The id here is a
keymap parameter the firmware can't enumerate, so this form advertises only the
bare `signal.fire` capability in the manifest ("I fire signals").

**Fixed form (devicetree-configured, editor-friendly + enumerated)** — like the
emit fixed form, the id lives on a zero-cell node, so it pastes into the **MoErgo
Glove80 Layout Editor** and, additionally, each node self-registers an enumerated
`signal.fire/<id>` entry in the manifest. A host scanning the keyboard then sees
*exactly which ids it fires* — e.g. `signal.fire/7`, `signal.fire/12` — instead of
an opaque "fires signals."

```dts
/ {
    behaviors {
        signal_7: signal_7 {
            compatible = "zmk,behavior-hid-signal-fixed";
            #binding-cells = <0>;
            id = <7>;
        };
    };
};
```

Then bind `&signal_7` on any key. Both forms share `CONFIG_HID_VIZ_SIGNAL` and the
same `[0xC0, id]` wire message.

**Security:** the host's id → action map is trusted, user-owned local config. The
wire only ever *references* a slot the user already defined; it can never define
one — so a keyboard can never inject an arbitrary command, only fire an id you
already mapped. Treat any device that can write the raw-HID report as able to fire
any configured id (fine for a local USB/BLE keyboard).

## Credits

Raw HID transport: [zmk-raw-hid](https://github.com/zzeneg/zmk-raw-hid) by zzeneg.
Notification approach inspired by [zmk-keypeek-layer-notifier](https://github.com/srwi/zmk-keypeek-layer-notifier) by srwi.
