# Changelog

## [Unreleased]
### Added
- Firmware 0.8.0-m1a: real device control in the web UI (Filtration/Lighting), replacing the demo for a
  recognised real device (family `pool_command_vs2`). `POST /api/v1/commands` now sends actual BLE commands
  when demo mode is off; new `GET /api/v1/real/status`. Device classification recognises `LRPC*` names and the
  product's service UUID. Confirmed working end to end against the real device (status read); starting the
  pump for real has been attempted but not yet confirmed due to BLE range/link stability - see docs/ble-auth.md.
- Firmware 0.7.1-m1a: `POST /api/v1/dev/ble/crypto` (AES-128-CTR encrypt/decrypt using the session nonce) to
  prepare/read encrypted BLE command frames. Used it to confirm, against a real POOL COMMAND VS2, that a read
  status command and a harmless write command (stopManualCommand) both round-trip correctly end to end -
  see docs/ble-auth.md for the exact bytes and the acknowledgement convention (response cmd = request cmd + 1).
- Firmware 0.7.0-m1a: BLE session authentication handshake (`POST /api/v1/dev/ble/auth`, AES-128-ECB nonce
  response) for Solem/Indygo devices that require it. The AES key is supplied locally via a git-ignored
  `ble_secrets.h` (see `ble_secrets.h.example`) and is never published or echoed by any endpoint. See
  docs/ble-auth.md, including results from testing it against a real POOL COMMAND VS².
- Firmware 0.6.2-m1a: developer tools - password-protected OTA update over Wi-Fi (verified 0.6.0 -> 0.6.2 on an
  ESP32-P4 board), Bluetooth explorer (connect, services, read, listen, write behind a time-limited lock, ring log),
  **Developer** web menu and an *Explore* button in the Bluetooth scan list. `tools/indygo_dev.py` command-line tool.
  Scan results now include `addrType`. See docs/developer-tools.md.
- Firmware 0.5.0-m1a: demo reshaped on the POOL COMMAND VS2 manual: filtration OFF/ON/AUTO (variable speed,
  thermo-adaptive, time ranges) with BOOST 24/48 h, speeds 1-3, up to 8 ranges; AUX1/AUX2 with equipment types,
  per-type modes, cover mode and safety interlocks. API: `PUT /filtration`, `PUT /aux1|aux2`; `/commands` now follows the
  same rules (`pause`/`backwash` rejected, boost 24/48 h only in AUTO). Replaces `PUT /pool`.
- docs/pool-command-vs2.md: summary of the POOL COMMAND VS² manual (hardware, 3-speed pump contacts, filtration
  and auxiliary modes, Bluetooth/LRMB control). docs/app-commands.md: Aquagem Modbus register map (bus-relay path).
- Firmware 0.4.0-m1a: Products menu (catalog of the app's product families), Pool and Lighting preview panels with
  the app's manual commands, CCEI/Seamaid colour values and filtration rules (demo mode). API: `/catalog`, `/sim`,
  `/commands`, `/pool`. The heat pump is deliberately left out for now.
- docs/app-commands.md: heat pump, light/colour and pool manual commands as found in the vendor app,
  with the transport each one uses (cloud vs local HTTP vs BLE).
- Demo mode (firmware 0.3.0-m1a): simulated pool controller with invented data to preview the full UI
  (Status with live measurements, Settings, banner). API: `/demo`, `/status`, `/config` (the last two only
  answer in demo mode until the real protocol is implemented; `503` otherwise).
- Firmware builds for the Waveshare ESP32-P4-WIFI6-DEV-KIT (`esp32p4`, `ChipVariant=postv3`); the access point
  was confirmed visible from a PC. A harmless C6 firmware-version warning appears in the boot log
  (see firmware/README.md).

### Fixed
- Access point name always ended in `0000` on boards with a co-processor radio; it now uses the eFuse MAC.
- Firmware 0.2.0-m1a (compiles, untested on hardware): BLE scan menu with Indygo/Solem device
  classification and saved selection; first-connection access point with captive portal; Wi-Fi menu to
  join the home network (credentials stored in flash); mDNS `indygo-gateway.local`.
- API: `/wifi`, `/wifi/scan`, `/ble/scan` (object with `scanning` + `devices`), `/device` (selection).
- Initial documentation: README, roadmap, BLE and vendor Wi-Fi protocol notes, methodology.
- Draft OpenAPI contract for the gateway REST API (`api/openapi.yaml`).
