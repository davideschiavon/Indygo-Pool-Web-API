# Firmware

ESP32 firmware: BLE discovery + web UI + REST API. Current version: **0.2.0-m1a**
(`indygo_gateway/indygo_gateway.ino`). It finds and remembers your device but does **not** talk to it
yet — that needs the protocol work in [../docs/ROADMAP.md](../docs/ROADMAP.md).

> Status: compiles for the classic ESP32. **Not yet tested on hardware** — please report what you see.

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
