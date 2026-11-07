# Roadmap and verification status

Legend: ✅ read directly in the vendor Android app (static analysis) · ❓ hypothesis, not verified
on hardware · ⬜ not started

## Findings

| # | Finding | Status |
|---|---------|--------|
| 1 | PoolTech devices advertise as `GEN_xxxx-xxxxxx-xxx` (or short `GEN_Vn…`); Sterilor EVO as `EVO_XXX_xxxx-xxxxxx-xxx` | ✅ |
| 2 | PoolTech BLE uses one service with separate RX/TX characteristics (see [protocol-ble](protocol-ble.md)) | ✅ |
| 3 | PoolTech frames are TLV with a CRC16 trailer; no payload encryption seen in the frame builder | ✅ (bonding/pairing not checked) |
| 4 | Vendor Wi-Fi modules expose a local HTTP/JSON API on port 80, discovered via mDNS | ✅ (see [protocol-wifi-local](protocol-wifi-local.md)) |
| 5 | The same endpoint layout exists on the vendor cloud | ✅ (auth not analysed) |
| 6 | Pool treatment devices ("IPX" family) reach the cloud through BLE + the phone, not via the Wi-Fi module API | ❓ |
| 7 | Which exact models the maintainers own / which tags each model supports | ⬜ |

## Milestones

1. **M0 – Research** (current): document the protocol, collect real device data.
   - **M1a – Discovery + onboarding** (code written, compiles, awaiting hardware test): first-connection
     access point, Wi-Fi setup menu, BLE scan menu, device selection. See [../firmware/README.md](../firmware/README.md).
2. **M1 – BLE read-only**: ESP32 connects to the selected device, reads status/measurements.
3. **M2 – Web UI + REST (read-only)**: implement `api/openapi.yaml` read endpoints.
4. **M3 – Write support**: setpoints / modes, behind an explicit opt-in flag and safety limits.
5. **M4 – Integrations**: Home Assistant (MQTT discovery), example clients.

## Maintenance rule

Every change to behaviour must update, in the same pull request: the relevant file in `docs/`,
`api/openapi.yaml` if the API changes, and `CHANGELOG.md`.
