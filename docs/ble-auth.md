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

## Tested against a real device (2026-11-07) — confirmed working end to end

Against an actual POOL COMMAND VS² (LRPC-VS2), using `POST /api/v1/dev/ble/crypto` (`encrypt-ctr`/`decrypt-ctr`,
AES-128-CTR-NoPadding, fresh counter = the session nonce for each message, matching the app's per-call Cipher
usage) together with the existing read/write/auth endpoints:

1. Performed the handshake (`/dev/ble/auth`): read a 16-byte nonce, sent back its AES-128-ECB encryption.
2. Encrypted the 2-byte "read pool status" command (`0xC7 0x00`, from `FrameManager.readPoolStatus()`), wrote
   it to the command characteristic (`91D50002-...`), and received a notification on `91D50003-...`.
   **Decrypted it to `C8 0C 01 0C 0C 14 0C 06 00 03 00 00 00 00`** — a clean response frame starting with
   `0xC8` (`0xC7 + 1`, the request-command-plus-one acknowledgement convention used throughout this protocol).
3. Encrypted and sent the 4-byte **write** command `stopManualCommand` for output 0 (`B3 02 01 00`, from
   `FrameManager.poolManualCommand()`) — harmless: it returns filtration to AUTO rather than starting
   anything. The decrypted notification response was **`B4 02 01 00`**: `0xB4 = 0xB3 + 1`, echoing back our
   exact `02 01 00` (length, action=stop, output index=0).

This is conclusive: the key, the nonce/CTR handling, and the command frame format are all correct, for both
reading status and sending a write command that the device parses and acknowledges. What has **not** been
tested yet is a command that actually changes the physical state of the equipment (e.g. `manualOn`, which
would start the filtration pump) — that is deliberately held back pending the owner's explicit go-ahead for
each such test, and the exact byte-level meaning of the status response fields above is not yet mapped.

## Responsible handling

- Never commit the key, a captured raw handshake that embeds it in a reversible way, or working exploit
  code targeting other people's equipment.
- This is documented for interoperability with equipment the maintainers own, consistent with
  [methodology.md](methodology.md).
