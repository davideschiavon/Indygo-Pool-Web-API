# Developer tools (firmware 0.6.x)

Purpose: let a developer update the gateway over Wi-Fi and explore a real device's Bluetooth services without a
USB cable, so the protocol work can be done with the ESP32 placed next to the equipment.

## Access model

- All developer endpoints need the header `X-Dev-Password`. **Until a password is set the tools are disabled.**
  The first password can be set by anyone who can reach the gateway (`PUT /api/v1/dev/password`), so set it right
  after flashing; changing it later needs the current one.
- The password is stored in the ESP32's flash as an unsalted SHA-256 hash.
- **Limits you should know about:** traffic is plain HTTP on your LAN (no TLS), so anyone able to sniff that
  network can capture the password; a short delay after a wrong password only slows guessing. Use the tools only on
  a network you trust and never expose the gateway to the internet.
- Whoever has the password can replace the firmware and send raw Bluetooth traffic to connected equipment.

## Firmware update (OTA)

`POST /api/v1/ota`, multipart field `file` = the application image `indygo_gateway.ino.bin` (not the merged,
bootloader or partitions files). The device installs it, then restarts. There is no automatic rollback: a bad
image needs a USB cable to recover. The partition scheme must have two app slots (*Minimal SPIFFS*).

```
arduino-cli compile --fqbn esp32:esp32:esp32p4 --board-options ChipVariant=postv3,PartitionScheme=min_spiffs --output-dir build firmware/indygo_gateway
python tools/indygo_dev.py ota build/indygo_gateway.ino.bin
```

Verified: 0.6.0 -> 0.6.1 -> 0.6.2 updated over Wi-Fi on a Waveshare ESP32-P4-WIFI6-DEV-KIT.

## Bluetooth explorer

Connect (demo mode must be off), list services/characteristics with their properties, read, listen for
notifications, and write. A ring log (80 entries) records connects, reads, notifications and **every write**.

- **Writes are locked by default.** `PUT /api/v1/dev/ble/write-enable` unlocks them for 1-30 minutes; they lock again
  on expiry or reboot. Only write frames you understand: unknown bytes can change equipment configuration.
- The connect call has a timeout; an unreachable address returns an error instead of hanging the web server.
- Addresses of many devices are *random* (resolvable private) and change; use the address type shown by the scan.

Web page: **Developer** menu (use *Explore* in the Bluetooth scan list). Command line: `tools/indygo_dev.py`
(standard library only; `--help` lists the commands). The password is taken from `--password`,
`INDYGO_DEV_PASSWORD` or `~/.indygo_dev_password`.

## Endpoints

| Method and path | Purpose |
|-----------------|---------|
| `GET /api/v1/dev` | Is a password set, BLE link state, write lock state (no password needed) |
| `PUT /api/v1/dev/password` | Set / change the password |
| `POST /api/v1/ota` | Install a firmware image |
| `POST /api/v1/dev/ble/connect`, `.../disconnect` | Connect to `{address, addrType}` / disconnect |
| `GET /api/v1/dev/ble/services` | Services and characteristics |
| `POST /api/v1/dev/ble/read`, `.../subscribe` | Read / listen on `{service, characteristic}` |
| `PUT /api/v1/dev/ble/write-enable` | Unlock writes for `minutes` |
| `POST /api/v1/dev/ble/write` | Write `{service, characteristic, hex, response}` (only when unlocked) |
| `GET /api/v1/dev/ble/log?since=N` | Log entries after sequence N |
