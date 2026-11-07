# Vendor Wi-Fi module local HTTP API (reference)

This is documented for **reference and possible future compatibility**. It belongs to the vendor's
Wi-Fi modules (irrigation/relay type). It is **not** confirmed to apply to pool treatment devices.
Source: static analysis of the vendor Android app. Not yet tested on hardware.

## Discovery

mDNS / Bonjour service types: `_solemv2._tcp`, `_solemwf._tcp`, `_jardibricwf._tcp`.
TXT record keys seen: `MAC`, `MID`, `MSN`, `LEVEL`, `DO`, `IS`. Default port: 80. ✅

## URL layout

| Mode | Base |
|------|------|
| Local, v2 | `http://<ip>/v2/module/<serial>/<endpoint>` |
| Local, v1 | `http://<ip>/v1/module/<webId>/<endpoint>` |
| Vendor cloud | `https://<vendor-host>/api/module/<serial>/<endpoint>` |

## Endpoints seen

`information`, `status`, `configuration`, `inventory`, `values`, `programs`, `relaycfg`,
`relaycontrol`, `relaystate`, `irricfg`, `irricontrol`, `irristate`, `installation`, `abortinst`,
`otafu`. ✅ (names only)

Request/response JSON uses short keys (e.g. `mtype`, `msn`, `mid`, `ssid`, `state`, `time`,
`rssi`, `battery`). Full schemas: TODO, to be filled from captures on real hardware.
