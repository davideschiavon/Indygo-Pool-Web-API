# How the findings were obtained

The protocol facts in this repository come from **static analysis of the vendor's publicly
distributed Android app on a device owned by the author**, for the purpose of interoperability
with that owner's own equipment.

## Rules for this repository

- **Do not commit** vendor APKs, decompiled source, extracted resources, firmware images, or
  any vendor-copyrighted material. `.gitignore` blocks the usual paths.
- Document **facts** (UUIDs, frame layouts, field meanings) in your own words, citing how you
  learned them (app version, capture date, device model).
- Label every claim **verified on hardware** or **from app analysis only**.
- Never publish serial numbers, MAC addresses, Wi-Fi credentials, tokens or account data.
- Do not publish methods to bypass the manufacturer's safety limits.

## App analysed

MyIndygo for Android, package `fr.solem.connected_pool` (version recorded when first analysed:
TBD by maintainers).
