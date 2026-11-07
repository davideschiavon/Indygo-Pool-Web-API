# Firmware

ESP32 firmware: BLE discovery + web UI + REST API. Current version: **0.4.0-m1a**
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

## Demo mode

On the **Status** page tick *Demo mode*. The gateway then behaves as if a pool controller were connected, so you
can explore every menu (live measurements, Settings with ranges and boost/winter/mode, Bluetooth, Wi-Fi) before the
real protocol is implemented. An orange banner is shown on every page while it is on.

**Everything in demo mode is simulated.** The values are invented, nothing is read from or sent to real
equipment, and the field set is *illustrative* (modelled on what the vendor app exposes for treatment units),
not a verified protocol. The choice is saved across reboots; untick the box to leave it. API: `GET/PUT /api/v1/demo`;
`/status` and `/config` answer `503` when no device is connected and demo mode is off.

## Products, Pool and Lighting (preview)

The **Products** menu lists the product families of the vendor app (BPC, Pool Box, Pool Guard, AC1, IPX treatment
devices, relays, ...). Entries marked *Preview* open a demo panel; the rest are listed but not supported yet. The
heat pump is listed as *not supported yet* on purpose.

With Demo mode on, **Pool** offers the app's filtration rule (time / thermo-adaptive / variable speed) and manual
commands (manualOn, boost, pause, backwash, stop); **Lighting** offers on/off, the colour pulse, and the CCEI
palette or Seamaid commands with the values the app uses. Everything is simulated; durations, limits and the
output mapping (index 0 = pump, 1 = light) are assumptions - see [../docs/app-commands.md](../docs/app-commands.md).
Third-party apps use the same JSON through `POST /api/v1/commands`.

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
