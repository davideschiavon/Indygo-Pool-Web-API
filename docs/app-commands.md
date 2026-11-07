# Commands in the vendor app, and how they reach the equipment

Source: static analysis of the vendor Android app (see [methodology](methodology.md)). Nothing here is tested on
hardware yet. Numbers and keys are facts read from the app; the *interpretation* is marked when uncertain.

> **Heat pump: set aside for now** (decision of the project owner). It is documented here for later; the
> firmware, API and UI do not implement it.

## 1. Key finding: three different paths

| Feature | Transport used by the app | Needs a vendor account? |
|---------|---------------------------|-------------------------|
| **Heat pump** (Polytropic): mode, power mode, setpoint | Vendor **cloud** (`ipxData`, see §2) | **Yes** |
| **Pool treatment readings** (PoolTech/IPX): pH, ORP, temperature, salt, … | **BLE**, read-only tags (see [protocol-ble](protocol-ble.md)) | No |
| **Lights / colour / pool manual commands** (on, boost, pause, stop, colour change, backwash) on Solem pool/lighting modules | `sendManualCommand`: **local Wi-Fi HTTP** (§3) **or BLE** (§4), chosen per module | No (local) |

Consequence for this project: a BLE-only ESP32 can read PoolTech measurements and, for Solem modules reachable
locally, send the manual commands. The heat pump is only reachable through the vendor cloud with the owner's
own account; whether it can be controlled locally is **unknown**.

## 2. Heat pump (cloud)

Modes (`PolytropicMode`): `0 OFF`, `1 AUTO`, `2 HEATING`, `3 COOLING`.
Power modes (`PolytropicPowerMode`): `0 Off`, `1 Eco`, `2 Smart`, `3 Boost`.
Other field seen: `temperatureSetpoint` (integer °C). The app keeps a *pending* copy of the output's
`ipxData` JSON, then sends it to the cloud:

`POST <vendor-base-url>/api/outputs/<outputId>/ipxData` with body `{"ipxData": { ... }}` and an
`Authorization` header carrying the signed-in user's token.

This document intentionally does not describe how accounts or tokens are obtained.
The BLE channel for the heat pump seems to be used only for pairing and giving it Wi-Fi credentials
(❓ inferred from the app's strings, not confirmed).

## 3. Manual commands over local HTTP (Solem Wi-Fi modules)

`POST http://<module-ip>/v2/module/<serial>/manual` with body `{"pool": { ... }}` (see
[protocol-wifi-local](protocol-wifi-local.md) for discovery). Fields: `index` (output number),
`action`, optional `manualDuration`, optional `speed` (variable-speed pumps only).

| App command | `action` | Extra fields |
|-------------|---------:|--------------|
| `stopManualCommand` | 1 | – |
| `manualOn` (timed) | 2 | `manualDuration` (duration), `speed` |
| `boost` | 3 | `time` = `HH:MM` (from `boostDuration`/60 s), `speed` |
| `colorChange` (pulse the light supply to step colour) | 4 | – |
| `pause` | 5 | `manualDuration` = days (`pauseDurationInDays`) |
| `colorChangeCCEI` (CCEI spotlights) | 9 | `manualDuration` = `(mode << 16) \| 96` |
| `colorChangeSeamaid` (Seamaid spotlights) | 10 | `manualDuration` = `mode << 24` |
| `backwash` | 11 | `manualDuration` = `duration + 168099840` |

`mode` is the colour/programme number chosen in the app (spotlight type decides CCEI vs Seamaid).
Units of `duration` are not yet confirmed.

`manualOn` also accepts `forcesFiltering`: when true the app sends two actions in one request - filtration
(output 0, `action 2`, duration + `additionalFilteringDuration`, default 600) and the output itself (`action 7`).

### CCEI spotlight values (`mode` of `colorChangeCCEI`)

Hot white 227, White 195, Blue 35, Lagoon 243, Cyan 115, Purple 51, Magenta 251, Pink 123, Red 59, Orange 225,
Green 97, Favorite 33, Light 25% 241, Light 50% 113, Light 75% 49, Gradient 121, Rainbow 57, Parade 224,
Techno 96, Horizon 32, Random 240, Magic 112, Speed 50 120, Speed 100 248, Speed 200 48, Save colour 56.
Seamaid `mode`: 16 change colour, 48 save colour, 32 reset. (The grouping into colours/levels/programs/speeds
is an interpretation of the app's UI code.)

### Filtration rules (`PoolProgramRule`)

`0` time, `1` thermo-adaptive, `2` variable-speed pump.

## 4. Manual commands over BLE (Solem modules)

The same commands are encoded as a short byte sequence: `[length-ish, action, index, …payload]`, for example
`colorChange` = `02 04 <index>`, `stop` = `02 01 <index>`, boost = `06|07 03 <index> <4-byte duration> [speed]`.
These bytes are only the *body*: the frame header, the service/characteristic UUIDs for the module type, and any
authentication/session handling still have to be extracted (open task; see ROADMAP).

## 5. What the app's "pool modes" are

- Pump/filtration programme rule (`PumpModeActivity`): three selectable rules (values 0, 1, 2); names TBD.
- Heat pump: the 4 modes and 4 power modes above.
- The pool outputs have manual commands (§3): on, boost, pause, stop.

❓ "4 modes" in the project brief is most likely the heat pump's 4 modes and/or 4 power modes. To be confirmed
against the owner's actual equipment.

## 6. Aquagem pump on an "IPX bus relay" (Modbus RTU) - a different path

The app also has an `aquagemPump` product: a **bus relay** talking **Modbus RTU** to the pump (preset "Aquagem",
slave address 1, preset mode code 3; serial parameters not given in the preset). This is NOT how a pump wired
to the POOL COMMAND VS² speed contacts is driven (see [pool-command-vs2](pool-command-vs2.md)); it is documented
because it would allow a direct RS485 link from an ESP32 if the owner's pump exposes it.

| Register | Name in preset | Access | Meaning (key) |
|---------:|----------------|--------|---------------|
| 3001 | RUNNING_CAPACITY_SETTING | write FC16 / read FC04 | speed percentage (`speedPercentage`), app slider 30-100 % in 5 % steps |
| 2001 | ERROR_CODE | read FC04 | `errorCode` (bit field; 16 alarm types: communication error, no-water protection, overload, ...) |
| 2002 | FREQUENCY_INVENTER | read FC04 | `runningState` |
| 2003 | RUNNING_CAPACITY | read FC04 | `runningCapacity` |
| 2004 | POWER | read FC04 | `power` |
| 2007 | POWER_CONSUMPTION | read FC04 | `powerConsumption` (scaled by 1/1000) |
| 2008 | MODE_CODE | read FC04 | `modeCode` |
| 2009 | SOFWARE_VERSION | read FC04 | `softwareVersion` |

Register numbering (0- or 1-based) and the pump's own serial settings are unverified.

## Open questions

- [x] Owner's equipment: POOL COMMAND VS² (LRPC) with an Aquagem IP20 filtration pump (3-speed contacts), per the owner.
- [ ] Is an LRMB antenna present? Which lighting/auxiliary equipment is on AUX1/AUX2?
- [ ] Does the heat pump accept local control (BLE/Wi-Fi), or is it cloud-only?
- [ ] Module-type BLE UUIDs, frame header and authentication for the manual commands.
- [ ] Units/limits for `manualDuration`, meaning of colour `mode` numbers per spotlight type.
