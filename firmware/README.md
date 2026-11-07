# Firmware

ESP32 firmware: BLE discovery + web UI + REST API. Current version: **0.8.0-m1a**
(`indygo_gateway/indygo_gateway.ino`). It finds and remembers your device but does **not** talk to it
yet — that needs the protocol work in [../docs/ROADMAP.md](../docs/ROADMAP.md).

> Status: compiles for the classic ESP32 and for the Waveshare ESP32-P4-WIFI6-DEV-KIT. Verified on the P4 board:
> access point visible (WPA2, channel 1), the saved home network is rejoined after a reboot/reflash, the
> hostname `indygo-gateway.local` resolves from a Windows PC, and the REST API (gateway, demo, status, config
> with range checks) behaves as documented. BLE scan results and the real device connection are still
> to be confirmed — please report what you see.

## Build and flash

Arduino core `esp32` 3.x, board *ESP32 Dev Module*, partition scheme **Minimal SPIFFS (1.9MB APP with OTA)**
(the BLE + Wi-Fi image is ~1.7 MB, too big for the default partition). Library needed: ArduinoJson 7.

```
arduino-cli compile --fqbn esp32:esp32:esp32 --board-options PartitionScheme=min_spiffs firmware/indygo_gateway
arduino-cli upload  -p COMx --fqbn esp32:esp32:esp32 --board-options PartitionScheme=min_spiffs firmware/indygo_gateway
```

## First connection

1. Power the ESP32. With no saved network it starts its own access point
   **`Indygo-Gateway-XXXX`** (XXXX = last digits of its MAC), password **`indygo-setup`**.
2. Join it with a phone or PC; the setup page opens by itself (captive portal), otherwise browse to
   `http://192.168.4.1`.
3. Open **Wi-Fi**, scan, pick your home network (2.4 GHz only), enter the password, *Save and connect*.
4. The gateway restarts and joins your network. Reconnect to your home Wi-Fi and open
   `http://indygo-gateway.local` (or the IP shown by your router).
5. If it cannot join within ~15 s it brings the access point back so you can retry; while the access
   point is idle it retries the saved network every minute. *Forget saved network* returns to step 1.

The home Wi-Fi password is stored in the ESP32's flash (never in the repository, never returned by the API).
There is no login: anyone on your LAN (or on the access point) can use the UI, so do not expose the gateway
to the internet. Change `AP_PASSWORD` in the sketch if you want a different setup password.

## Finding your pool device

Open **Bluetooth devices → Scan**. Devices that look like Indygo/Solem pool equipment are highlighted
(`pooltech`, `sterilor_evo`, `solem_ipx`), with their advertised name, services and manufacturer data.
Select yours; the choice is saved. **Please share what your device shows** (without serial/MAC) in an issue —
it is the fastest way to finish the protocol notes.

## Updating without USB, and the Bluetooth explorer

Once the firmware with developer tools is on the board, later versions can be installed over Wi-Fi, and a real
device's Bluetooth services can be explored from the **Developer** menu or `tools/indygo_dev.py`. Read
[../docs/developer-tools.md](../docs/developer-tools.md) first (password, limits, write lock).

## Demo mode

On the **Status** page tick *Demo mode*. The gateway then behaves as if a pool controller were connected, so you
can explore every menu (live measurements, Settings with ranges and boost/winter/mode, Bluetooth, Wi-Fi) before the
real protocol is implemented. An orange banner is shown on every page while it is on.

**Everything in demo mode is simulated.** The values are invented, nothing is read from or sent to real
equipment, and the field set is *illustrative* (modelled on what the vendor app exposes for treatment units),
not a verified protocol. The choice is saved across reboots; untick the box to leave it. API: `GET/PUT /api/v1/demo`;
`/status` and `/config` answer `503` when no device is connected and demo mode is off.

## Preview of the POOL COMMAND VS2 (demo mode)

The **Products** menu lists the product families of the vendor app. The entry *POOL COMMAND VS2 (BPC / LRPC)*
opens the preview; the heat pump is listed as *not supported yet* on purpose.

With Demo mode on:

- **Filtration** follows the manufacturer's manual: OFF, ON (forced, speed 1-3), AUTO (variable-speed ranges,
  thermo-adaptive with a +/-4 h daily adjustment, or time ranges for single-speed pumps) and BOOST 24/48 h
  (AUTO only). Up to 8 daily ranges with weekdays and speed.
- **Auxiliaries** AUX1/AUX2: choose the equipment (lighting, treatment, robot, other) like in the app, then use the
  modes that equipment has. Safety interlocks are modelled: a robot ON forces filtration, treatment accepts only OFF
  and AUTO, and cover mode (0-60 min/h) is available for treatment.
- **Lighting** works on the auxiliary set to *lighting*: on/off, colour pulse, and the CCEI palette or Seamaid
  commands with the values the app uses.

Everything is simulated; schedules are stored but not evaluated (no clock). Durations, limits and the output mapping
(index 0 = filtration, 1 = AUX1, 2 = AUX2) are assumptions - see [../docs/app-commands.md](../docs/app-commands.md)
and [../docs/pool-command-vs2.md](../docs/pool-command-vs2.md). Third-party apps use `PUT /api/v1/filtration`,
`PUT /api/v1/aux1|aux2` and the app-style `POST /api/v1/commands` (see `api/openapi.yaml`).

## BLE session authentication (developer tools)

Some Solem/Indygo devices (confirmed on a POOL COMMAND VS²) require a nonce/AES handshake before their data
characteristics are usable - see [../docs/ble-auth.md](../docs/ble-auth.md). The gateway can perform this
handshake (`POST /api/v1/dev/ble/auth`) if a local, git-ignored `ble_secrets.h` supplies the key (copy
`ble_secrets.h.example`); **the key itself is not published in this project**. Tested against a real device:
the full pipeline is **confirmed working** - a read-status command and a harmless write command
(`stopManualCommand`) were both sent encrypted and acknowledged correctly. Actually changing equipment state
(e.g. starting the pump) is held back pending explicit owner go-ahead for each test. See the doc.

## Real device control (replaces the demo for a supported device)

For a device of family `pool_command_vs2` (the POOL COMMAND VS2/LRPC-VS2 confirmed in docs/ble-auth.md), the
**Filtration** and **Lighting** pages show a red-bordered **REAL control** box instead of relying on the
demo: manual ON/stop/boost for filtration, on/off/colour-pulse/CCEI/Seamaid for lighting (pick AUX1 or AUX2).
These send actual encrypted BLE commands - no demo mode, no developer password. The schedule/mode editors
below them remain demo-only (not yet validated byte-for-byte against real hardware). BLE range to the real
device matters: a weak signal (worse than about -85 dBm in testing) can make a command fail to reach the
device - the UI reports this honestly rather than pretending success. See
[../docs/ble-auth.md](../docs/ble-auth.md) for exactly what has been confirmed.

## ESP32-P4 notes (Waveshare ESP32-P4-WIFI6-DEV-KIT)

The P4 has no radio of its own; Wi-Fi and Bluetooth come from an on-board ESP32-C6 over SDIO (ESP-Hosted).
The same sketch compiles unchanged (about 1.0 MB):

```
arduino-cli compile --fqbn esp32:esp32:esp32p4 --board-options ChipVariant=postv3,PartitionScheme=min_spiffs firmware/indygo_gateway
```

Use `ChipVariant=postv3` for chip revision v3.x (check with `esptool chip-id`). The core's default SDIO pins
(CLK 18, CMD 19, D0-D3 14-17, C6 reset 54) match this board.

**Boot log note (observed 2026-11-07):** the log shows
`rpc_core: Response not received for [0x15e](Req_GetCoprocessorFwVersion)`. This is only the host asking the
C6 for its ESP-Hosted firmware version (an older slave firmware does not answer it); Wi-Fi works anyway — the
access point is visible. If you see real Wi-Fi/BLE failures, updating the C6 slave firmware (esp-hosted-mcu,
matching the core's host version) is the next thing to try; see the Waveshare wiki FAQ (C6 download mode:
pull `C6_IO9` low at power-on and use the C6 UART pads).

Note: `WiFi.macAddress()` can read zeros early on this board, so the access point name uses the chip's eFuse MAC
(`Indygo-Gateway-XXXX`, XXXX = last two bytes).
