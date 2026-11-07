# Contributing

Everyone is welcome — hardware owners, firmware developers, documentation writers.

## Ways to help

- Report your device: model, advertised BLE name, firmware version (no serial numbers).
- Share BLE captures (nRF Connect logs, HCI snoop logs) with identifiers removed.
- Fill the open questions in [docs/protocol-ble.md](docs/protocol-ble.md).
- Implement firmware milestones from [docs/ROADMAP.md](docs/ROADMAP.md).
- Build clients/integrations against [api/openapi.yaml](api/openapi.yaml).

## Rules

1. **Keep docs in sync.** A pull request that changes behaviour must also update `docs/`,
   `api/openapi.yaml` (if the API changes) and `CHANGELOG.md`.
2. **Mark certainty.** Say whether a claim is verified on hardware or from app analysis only.
3. **No vendor material.** Do not commit APKs, decompiled code, firmware images or copied assets
   (see [docs/methodology.md](docs/methodology.md)).
4. **No secrets or personal data:** serial numbers, MAC addresses, Wi-Fi passwords, tokens.
5. **Safety first.** Any write/control feature must respect the manufacturer's limits and be
   opt-in.
6. Discuss larger changes in an issue before opening a pull request.
