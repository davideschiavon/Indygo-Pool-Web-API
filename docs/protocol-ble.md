# BLE protocol notes

Source: static analysis of the vendor Android app (MyIndygo). Everything below marked ✅ was read
in the app's code; nothing has been confirmed on a device yet.

## Device identification (advertising)

| Family | Advertised local name pattern | Status |
|--------|-------------------------------|--------|
| PoolTech | `GEN_[0-9]{4}-[0-9]{6}-[0-9]{3}…` or `GEN_V[0-9]…` | ✅ |
| Sterilor EVO | `EVO_[A-Z]{3}_[0-9]{4}-[0-9]{6}-[0-9]{3}…` | ✅ |
| Other IPX devices | Service UUID `84A4000x-C06B-4D49-87BC-EDBBAE1F9EAB` (x = variant index); model in manufacturer data bytes 2–3, device state in byte 10 | ✅ |

## PoolTech GATT

| Role | UUID | Status |
|------|------|--------|
| Service | `09912756-7B32-4629-AEB1-B309D9A338AE` | ✅ |
| RX characteristic | `EF785C24-22BB-463D-B651-0B7445BA091C` | ✅ |
| TX characteristic | `4D32C5E5-2BB5-45C6-8C89-6F59BB3930D2` | ✅ |

Direction naming (RX/TX relative to the phone or to the device) is **not yet confirmed** — check
on hardware.

## Frame format

- TLV-style frames: type, tag, length, value, followed by a **CRC16** (two bytes, high then low).
  ✅ (a CRC16 routine and a TLV builder exist; the exact polynomial/initial value and the numeric
  tag table still need to be extracted — tracked as an open task.)
- Known read operations in the app: firmware version, model version, bootloader version, MAC
  address, configuration code, local name, log events. ✅ (names only; tag numbers TBD)

## Open questions

- [ ] CRC16 variant (poly/init/reflection).
- [ ] Numeric tag table and measurement encoding (pH, ORP, salt, setpoints, modes).
- [ ] Is bonding/pairing or an application-level challenge required before commands are accepted?
- [ ] Can two BLE clients be connected at once (phone app + ESP32)?
- [ ] Exact behaviour per model (PoolTech vs Sterilor EVO vs others).

Contributions: add captured traffic (with identifiers removed) under `docs/captures/`.
