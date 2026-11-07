# Indygo-Pool-Web-API

An open-source **ESP32 gateway** that talks to Indygo / Solem connected pool equipment over
**Bluetooth Low Energy** and exposes it on your local network as a **web page and a REST API** —
no cloud account, no login.

> **Status: early research / draft.** Nothing here is a finished product yet. See the
> [roadmap](docs/ROADMAP.md) for what is verified and what is still a hypothesis.

## Goals

1. ESP32 connects to the pool device over BLE (client role).
2. ESP32 serves a configuration/monitoring web page on the LAN.
3. ESP32 exposes a documented REST API (+ live events) so **third-party applications**
   (Home Assistant, Node-RED, scripts, mobile apps…) can integrate.
4. Everything stays local. No credentials, no vendor cloud.

## Repository layout

| Path | Content |
|------|---------|
| `docs/` | Protocol notes, architecture, roadmap |
| `api/openapi.yaml` | **Draft** REST API for third-party apps (the gateway's contract) |
| `tools/` | `indygo_dev.py`: command-line tool for OTA updates and the Bluetooth explorer |
| `firmware/` | ESP32 firmware: first-connection access point, Wi-Fi + Bluetooth-scan web menus (see its README) |

## Documentation

- [Roadmap and verification status](docs/ROADMAP.md)
- [POOL COMMAND VS² manual summary](docs/pool-command-vs2.md)
- [Commands in the vendor app and their transports](docs/app-commands.md)
- [BLE protocol notes](docs/protocol-ble.md)
- [Vendor Wi-Fi module local HTTP API (reference)](docs/protocol-wifi-local.md)
- [How the findings were obtained](docs/methodology.md)
- [Developer tools: OTA update and Bluetooth explorer](docs/developer-tools.md)
- [Draft gateway API](api/openapi.yaml)

## Contributing

Anyone is welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md). The most valuable contribution right
now is **BLE captures and device information from real hardware** (model, advertised name,
firmware version) — please never post serial numbers or Wi-Fi credentials.

## Disclaimer

This is an independent, unofficial community project, not affiliated with or endorsed by Solem,
Indygo, PoolTech or Sterilor. Names are used only to identify compatible equipment.
It is intended for interoperability with equipment **you own**. Controlling pool chemistry
equipment can be dangerous: use at your own risk, keep the manufacturer's safety limits, and do
not rely on this project for safety-critical functions.

## License

MIT — see [LICENSE](LICENSE).
