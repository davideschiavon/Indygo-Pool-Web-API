# POOL COMMAND VS² (connected pool clock) - what the manual says

Source: the manufacturer's French user manual *POOL COMMAND VS² - Manuel d'installation et d'utilisation*, v1.4
(<https://support.myindygo.com/wp-content/uploads/2024/05/POOL-COMMAND-VS2-MANUEL-UTILISATEUR-FR.v1.4.pdf>).
This page is a summary in our own words; the manual is the authority. Wiring diagrams in the manual are images
and are not summarised here. The device appears in the vendor app as `LRPC-XXXXX` (category `lrpc`, shown as
"BPC" in some builds).

## Hardware

- Module version (DIN rail, not waterproof) or enclosure version (IP65 box with breakers, 12 V lamp transformer).
- 230 VAC input; one relay for treatment equipment; **dry-contact digital inputs that select the speed of a
  variable-speed filtration pump (3 speeds)**; relays **AUX1** and **AUX2** (10 A); PT100 water-temperature probe
  input; dry-contact input for a pool cover/shutter sensor (or flow sensor, empty-tank sensor).
- Pumps listed with wiring diagrams: Hayward, Pentair, Speck, Zodiac, Espa, DAB, **Aquagem**, Flowdians, Davey,
  Astral. Single-speed pumps are also supported (time-range mode).
- Radios: Bluetooth Low Energy 4.0 and LoRa (868 MHz).

## How it is controlled

- **Locally over Bluetooth** from the phone app, and/or **remotely** through a separate **LRMB-POOL / LRMB-10 /
  LRMB-30** antenna (LoRa to the clock, Wi-Fi 2.4 GHz to the internet) and the vendor cloud (account required).
- Pairing of Bluetooth treatment devices ("IPX") through the clock is only possible when an LRMB antenna is
  present; their measurements are collected about every 30 minutes and travel via the cloud.

## Operating modes (as described by the manual)

**Filtration**

| Mode | Meaning |
|------|---------|
| OFF | Filtration stopped |
| ON | Forced continuous run at the selected speed |
| AUTO - variable speed | Up to 8 daily ranges, each with its own speed, assignable to weekdays; ranges can be used for skimming with "continuous filtration" |
| AUTO - thermo-adaptive | Daily duration follows the water temperature (standard cycle, or -10/-25/-35/-50 % variants); "daily adjustment" of up to +/-4 h |
| AUTO - time ranges | For single-speed pumps: up to 8 daily ranges |
| BOOST | Only in AUTO: force filtration for 24 h or 48 h, then return to the previous AUTO mode |
| Frost protection | Automatic: water < 4 degC runs 15 min every 2 h; < 1 degC runs continuously |

**Auxiliaries (AUX1 / AUX2)** - each has its own mode set, chosen in the app's configuration
(lighting, heat pump, treatment/electrolyser, hydraulic robot, other):

- Lighting: OFF, AUTO with time ranges (plus manual commands in the app).
- Heat pump (set aside in this project for now): OFF, ON (forces filtration), AUTO by swimming time ranges.
- Treatment: OFF, AUTO (runs only while filtering; optional cover mode with a 0-60 min/h rate when the cover is closed).
- Robot: OFF, ON (forces filtration), AUTO by time ranges.
- Other equipment: OFF, ON, AUTO by time ranges.
- Starting a heat pump or robot automatically starts filtration (unless filtration is OFF); treatment equipment
  only runs while filtration runs.

Other settings: configuration (what is on AUX1/AUX2, temperature sensor source, cover sensor polarity), firmware
update, remote-access test. Alerts (pH, ORP, temperature) are configured on the vendor web platform.

## What this means for the project

- The **3 speeds** match the `speed` values 1-3 the app sends for variable-speed pumps.
- The filtration rules in the app (`time`, `thermo-adaptive`, `variable speed`) are the AUTO modes above.
- The VS² speaks Bluetooth LE locally, so a local ESP32 gateway is plausible for the manual commands - but the BLE
  service UUIDs, frame header and any pairing/authentication still have to be extracted (open task).
- **Safety:** pump, heat pump, electrolyser and robot interlocks are the manufacturer's. A gateway must never
  bypass them (e.g. never run treatment equipment without filtration).
