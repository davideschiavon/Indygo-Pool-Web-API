# Firmware

ESP32 firmware: BLE discovery + web UI + REST API. Current version: **0.2.0-m1a**
(`indygo_gateway/indygo_gateway.ino`). It finds and remembers your device but does **not** talk to it
yet — that needs the protocol work in [../docs/ROADMAP.md](../docs/ROADMAP.md).

> Status: compiles for the classic ESP32 and for the Waveshare ESP32-P4-WIFI6-DEV-KIT. On the P4 board the
> firmware boots, but the on-board ESP32-C6 radio did not answer (see *ESP32-P4 notes*), so Wi-Fi/BLE are
> **not working there yet**. Not yet confirmed working on any hardware — please report what you see.

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

## ESP32-P4 notes (Waveshare ESP32-P4-WIFI6-DEV-KIT)

The P4 has no radio of its own; Wi-Fi and Bluetooth come from an on-board ESP32-C6 over SDIO (ESP-Hosted).
The same sketch compiles unchanged (about 1.0 MB):

```
arduino-cli compile --fqbn esp32:esp32:esp32p4 --board-options ChipVariant=postv3,PartitionScheme=min_spiffs firmware/indygo_gateway
```

Use `ChipVariant=postv3` for chip revision v3.x (check with `esptool chip-id`). The core's default SDIO pins
(CLK 18, CMD 19, D0-D3 14-17, C6 reset 54) match this board.

**Known issue (observed 2026-11-07):** at boot the log shows
`rpc_core: Response not received for [0x15e](Req_GetCoprocessorFwVersion)` and the MAC reads as zeros, i.e. the
C6 is not answering as an ESP-Hosted slave, so no access point is visible. Most likely the C6 has no (or an
incompatible) ESP-Hosted slave firmware. `ESP_HostedOTA` cannot fix this because it needs a working link first;
the C6 firmware has to be flashed another way. Help welcome.
