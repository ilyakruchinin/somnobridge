# SomnoBridge

> A pocket-sized **WiFi ↔ BLE bridge** that lives permanently on an EZShare WiFi SD card's access point and serves its files to [SomnoTrace](https://github.com/ilyakruchinin/SomnoTrace) over Bluetooth Low Energy — so SomnoTrace never has to leave your home WiFi to fetch them.

Created and architected by **Ilya Kruchinin** ([@ilyakruchinin](https://github.com/ilyakruchinin)). Companion device to SomnoTrace.

---

## Why

SomnoTrace talks to your home network for uploads, dashboards and alerts — but EZShare WiFi SD cards are **AP-only**: they host their own network (`ezshare.card`, typically `192.168.4.1`) and never join yours. Pulling a file means SomnoTrace must drop its home WiFi association, join the card's network, download, and hop back — every time.

SomnoBridge eliminates the network switching. A second, cheap ESP32-S3 device stays **permanently associated with the EZShare AP**, passively observes the AirSense's DATALOG folders, estimates the machine's clock drift, and exposes a catalog + file streaming over an authenticated BLE link:

```
┌──────────┐   WiFi (STA)    ┌────────────┐    BLE 5 (GATT)    ┌─────────────┐
│ EZShare  │ ◄─────────────► │ SomnoBridge│ ◄────────────────► │ SomnoTrace  │
│ SD card  │   HTTP /dir +   │  (GEEK)    │   metadata + file  │             │
│  in AS10 │    /download    │            │    streaming       │             │
└──────────┘                 └────────────┘                    └──────┬──────┘
                                                                     │ home WiFi
                                                             uploads, dashboard,
                                                             MQTT, NTP — never drops
```

SomnoTrace stays on your LAN the whole time. The BLE link only carries data to one place.

---

## Hardware

Targets the **[Waveshare ESP32-S3-GEEK](https://www.waveshare.com/product/esp32-related/boards-kits/esp32-s3/esp32-s3-geek.htm)** — a USB-A stick form-factor board that hides behind the nightstand powered from any USB port:

| Peripheral | Part | Notes |
|---|---|---|
| MCU | ESP32-S3R2 (dual-core LX7 @ 240 MHz) | 512 KB SRAM, **2 MB PSRAM**, **16 MB flash**, WiFi + BLE 5 |
| Display | 1.14" 240×135 IPS LCD (ST7789) | Status output + pairing passkey display |
| Button | BOOT (GPIO0) | Pairing window / setup mode entry |
| Power | USB-A male | Wall-powered infrastructure, no battery |
| Debug | USB Serial/JTAG (`/dev/ttyACM*`) | Console + flashing |

The TF card slot is **unused** — file data streams through a bounded PSRAM ring, no staging cache. SomnoTrace remains the Waveshare ESP32-S3-Touch-LCD-1.54; it is the BLE central/client here.

## What the firmware does

1. **Provision** — holding BOOT enters SETUP mode: the device hosts a short-lived open setup AP (`SomnoBridge-Setup-XXXX`) with a configuration portal at `192.168.8.1`. You select the EZShare SSID (scanned list) and enter its password there. Settings persist in NVS; destructive actions require a physical button confirmation.
2. **Join** — connects to the EZShare AP as a STA with jittered backoff reconnect. An open/weak card WiFi is reported as a warning, never silently joined.
3. **Observe** — a scheduler polls `A:\DATALOG` on a bounded budget (≈1 listing/s sustained, ~5 s cadence for hot folders), maintaining a volatile in-memory catalog of day folders and file metadata. The same parsed listing feeds therapy detection — no subsystem polls the card independently.
4. **Estimate drift** — the AirSense clock drifts and EZShare has no usable clock, so SomnoBridge measures listing growth events and file timestamps against NTP anchors supplied by SomnoTrace over BLE, and publishes a qualified offset/drift estimate (best-effort, statistically plausible — see `docs/proposal-final.md`).
5. **Serve** — a NimBLE GATT server (LE Secure Connections, MITM passkey, encrypted link) exposes a JSON metadata API plus a credit-windowed file streaming channel. Full protocol: [`docs/ble-api.md`](docs/ble-api.md).
6. **Report** — the LCD shows EZShare link state, BLE link state, RSSI, therapy gate state and transfer progress; the BOOT button opens a 120 s pairing window (short press, unbonded) or SETUP (≥3 s hold).

## Transfer design

BLE is the bottleneck, so the design minimizes its cost:

- **2M PHY + DLE + 247-byte ATT MTU**, short connection interval — notifications stream with no per-chunk round trip.
- **Right-edge credit window**: the client grants `{offset, window}` via write-without-response; the sender transmits while `sent < offset + window` (~11.25 KiB in flight, sized to the NimBLE mbuf pool).
- **Optional compressed wire (v2, opt-in per request)**: raw bytes are block-encoded — `deflate` or `delta16+deflate` with raw passthrough fallback — so compressible EDF payloads move at ~50 KiB/s wire ≈ **235 KiB/s restored**; incompressible data runs at ~92–98 KiB/s wire. Legacy clients keep the uncompressed v1 wire (~99–106 KiB/s).
- **Read-ahead**: WiFi prefetch over a keep-alive HTTP session fills the PSRAM ring while BLE drains it.
- **Integrity**: SHA-256 over raw bytes in `STREAM_END`; frames carry sequence numbers; therapy gating blocks live-file access while the card may be appending.

Measured on hardware (2 MiB streams, concurrent-subscriber worst case, SHA-verified): ramp v2 52.4 KiB/s wire / 235 KiB/s raw, rand v2 91.7 KiB/s, rand v1 98.5 KiB/s. See `docs/implementation-update7-spec.md` §13.

## Security model (summary)

- One bonded peer. LE Secure Connections + MITM passkey shown on the LCD; pairing only inside a physically opened 120 s window.
- All characteristics gate on an encrypted link + authorized bonded identity; control/data delivery requires CCCD subscription.
- Settings, bonds and the evidence journal live in NVS; boards provisioned with an eFuse HMAC key use HMAC-backed NVS encryption, unprovisioned boards run `DEV-UNPROTECTED` (plain NVS) by default — set `CONFIG_SOMNOBRIDGE_DEV_UNPROTECTED=n` for a strict fail-closed profile.
- No HTTP listener in normal operation — the portal only exists inside SETUP mode.

Details: `docs/implementation-spec.md` §3–§4.

## Build & flash

Docker is the only host dependency; ESP-IDF is pinned to `v5.5.5`:

```bash
./scripts/idf.sh build                # idf.py build inside espressif/idf:v5.5.5
./scripts/idf.sh -p /dev/ttyACM0 flash
./scripts/build-dist.sh               # merged image into dist/
```

**GEEK flashing quirks** (details in `AGENTS.md`): the board may stay in download mode after `flash` — run any esptool op with `--after watchdog_reset` to boot the app, and never keep a serial reader open on the port while flashing. The port re-enumerates between `ttyACM0`/`ttyACM1` on reset; `/dev/serial/by-id/` is stable.

## Host tooling

`scripts/ble_query.py` is a bonded-host diagnostic + reference client (BlueZ/D-Bus):

```bash
./scripts/ble_query.py                       # status + catalog + days + drift
./scripts/ble_query.py --status --catalog
./scripts/ble_query.py --days 14 --day 20261005
./scripts/ble_query.py --ls '/'              # live card root listing
./scripts/ble_query.py --get '/DATALOG/20261005/20261006_002312_BRP.edf'
./scripts/ble_query.py --evidence            # evidence journal replay
./scripts/ble_query.py --time-sync 5         # NTP-style anchor exchange
./scripts/ble_query.py --stream-test 2097152 --pattern ramp   # add --v1 for legacy wire
./scripts/ble_query.py --self-test           # offline protocol self-tests
```

Host-side pitfalls it already encodes (see `docs/ble-api.md` §8): drop backward-sequence compact frames (BlueZ double-delivers when a second process holds `StartNotify`), ignore response fragments on a completed request id, and treat the Intel AX200 as fragile under sustained notify load.

## Tests

```bash
./scripts/run_host_tests.sh                  # pure-logic C tests, stub ESP headers
python3 scripts/ble_query.py --self-test     # framing/reassembly self-tests
python3 scripts/validate_timing.py self-test # timing/journal logic
```

## Repository layout

- `main/` — thin app entry / mode machine
- `components/` — `board` (LCD/button), `settings` (NVS), `netw` (STA/AP+portal), `ezshare` (HTTP adapter + parsers), `catalog` (volatile DATALOG index), `scheduler` (card worker + therapy FSM), `ble_api` (NimBLE GATT + framing + ops + codec), `drift` (anchors/fit), `journal` (bounded NVS evidence ring)
- `docs/` — specifications; `implementation-spec.md` is authoritative where docs disagree
- `scripts/` — `idf.sh` docker wrapper, host tools and tests

## Documents

- [`docs/ble-api.md`](docs/ble-api.md) — **BLE wire protocol + op reference** (what a client needs to talk to this device).
- [`docs/implementation-spec.md`](docs/implementation-spec.md) — v1 implementation contract (authoritative).
- `docs/implementation-update{,2,3,4,5,6,7}-spec.md` — amendment series; update7 covers the compressed v2 stream, credit semantics, mbuf sizing and host-dedup requirements.
- [`docs/as10-time-drift.md`](docs/as10-time-drift.md) — the clock-drift problem statement and evidence channels.
- [`docs/proposal-final.md`](docs/proposal-final.md) — the drift-estimation design (supersedes `proposal.md`).
- [`docs/speed-optim.md`](docs/speed-optim.md) — BLE throughput analysis and options considered.

## Licensing

GPLv3 with the same §7(b) attribution term as SomnoTrace — see `LICENSE` and `NOTICE`. SomnoBridge is a separate firmware, not a SomnoTrace fork: protocol documentation may inform behavior, but no SomnoTrace source is copied (clean-room rule in `AGENTS.md`).

## Related

- [SomnoTrace](https://github.com/ilyakruchinin/SomnoTrace) — the CPAP/oximetry recorder this bridges for
- EZShare protocol notes: `spec/archive/files-over-http.md` in the SomnoTrace repo
