# BLE session authentication (Solem/Indygo devices)

Source: static analysis of the vendor Android app (`BleManager` class). Confirmed by connecting the ESP32
gateway's developer BLE explorer to a real **POOL COMMAND VS² (LRPC-VS2)** and finding the exact
characteristics the app's formula predicts.

> **The key itself is intentionally not published here or anywhere in this repository.** It is a single
> AES-128 key shared by every device of this kind, embedded in the vendor app. Publishing it would let
> anyone's phone or script talk to anyone's equipment. See [ble_secrets.h.example](../firmware/indygo_gateway/ble_secrets.h.example)
> for how a maintainer who has extracted it from their own licensed copy of the app supplies it locally
> (git-ignored, never committed).

## Characteristic layout

Each product type has a service UUID of the form `91D5000x-1040-4F54-95B2-5FC31BE56385` (x = a small
integer that depends on the product type; `1` for the POOL COMMAND VS²). Within that service:

| Characteristic | Properties (observed) | Role |
|-----------------|------------------------|------|
| `91D50002-...` | write, write-without-response | commands to the device |
| `91D50003-...` | notify | data pushed by the device |
| `91D50005-...` | read, write, notify | **authentication** |

(Exact index-to-role mapping for other product families may differ; this is confirmed only for the
POOL COMMAND VS² service above.)

## Handshake

1. Connect and read the authentication characteristic.
2. If the value is the ASCII marker `AUTHENTICATED` followed by a zero byte (hex
   `41555448454e5449434154454400`), the device already trusts this session (it remembers the phone's BLE
   address from a previous handshake). The client's own record of the nonce it used last time for that
   address is reused; no further write is needed.
3. Otherwise, the value just read **is the nonce** (a fresh 16-byte challenge for this connection).
4. The client encrypts that nonce with **AES-128, ECB mode, no padding**, using the shared key, and writes
   the 16-byte result back to the same characteristic. The device verifies it and the session becomes
   authenticated.
5. The nonce from step 2/3 is then also the basis for payload encryption: subsequent reads/writes on the
   device's data characteristics are encrypted with **AES-128, CTR mode, no padding**, using the same key
   and that nonce. The exact counter convention (how the nonce seeds the CTR counter block) is **not yet
   confirmed** against a captured real payload — treat any CTR implementation as unverified until checked
   against traffic from the real device.

## What the gateway implements today

`POST /api/v1/dev/ble/auth` (developer tools, see [developer-tools.md](developer-tools.md)) performs steps
1-4 above against the currently connected device, using the key from `ble_secrets.h` if present. It does not
yet decrypt/encrypt data payloads (step 5) — that is the next piece of protocol work, blocked on confirming
the CTR convention from a real captured frame.

If `ble_secrets.h` is absent or empty, the endpoint returns a clear error instead of silently failing.

## Tested against a real device (2026-11-07)

Against an actual POOL COMMAND VS² (LRPC-VS2):

- **Confirmed**: the characteristic layout above (service `91D50001-...`, auth characteristic `91D50005-...`,
  write/notify characteristics `91D50002-...`/`91D50003-...`), reading a fresh 16-byte nonce, encrypting it
  AES-128-ECB-NoPadding and writing the 16-byte result back — all complete without an ATT error and without
  the device dropping the link (confirmed the link stayed up for 5+ seconds afterward and kept answering other
  reads).
- **Not yet confirmed**: whether the key/response was actually *accepted* as valid by the device. On a clean
  disconnect + reconnect, the auth characteristic always returned a fresh nonce rather than the `AUTHENTICATED`
  marker, and subscribing to the notify characteristic produced no traffic in a 20 s window post-handshake.
  The marker-reuse behaviour in the app may depend on OS-level BLE bonding (pairing), which this gateway does
  not perform yet — so its absence here is not proof the key is wrong, just not a positive confirmation either.
- **Next checks**: (a) perform an actual BLE bond/pair from the gateway and repeat the reconnect test; (b)
  implement the AES-CTR payload decryption (step 5) and see if a read on the data characteristic yields
  plausible values (e.g. a temperature in a sane range) instead of noise.

## Responsible handling

- Never commit the key, a captured raw handshake that embeds it in a reversible way, or working exploit
  code targeting other people's equipment.
- This is documented for interoperability with equipment the maintainers own, consistent with
  [methodology.md](methodology.md).
