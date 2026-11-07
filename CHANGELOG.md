# Changelog

## [Unreleased]
### Added
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
