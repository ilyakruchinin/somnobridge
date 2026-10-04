# SomnoBridge

> A pocket-sized **WiFi ↔ BLE bridge** that sits permanently on an EZShare WiFi SD card's access point and serves its files to [SomnoTrace](https://github.com/ilyakruchinin/SomnoTrace) over Bluetooth Low Energy — so SomnoTrace never has to leave your home WiFi to fetch them.

Created and architected by **Ilya Kruchinin** ([@ilyakruchinin](https://github.com/ilyakruchinin)). Companion device to SomnoTrace.

---

## Why

SomnoTrace talks to your home network for uploads, dashboards and alerts — but EZShare WiFi SD cards are **AP-only**: they host their own network (`ezshare.card`, typically `192.168.4.1`) and never join yours. Pulling a file means SomnoTrace must drop its home WiFi association, join the card's network, download, and hop back — every time.

SomnoBridge eliminates the network switching entirely. It is a second, cheap ESP32-S3 device that lives **permanently on the EZShare network**, and exposes the card's files over a fast BLE link to SomnoTrace:

```
┌──────────┐   WiFi (STA)   ┌───────────┐    BLE 5 (GATT)   ┌─────────────┐
│ EZShare  │ ◄────────────► │ SomnoBridge  │ ◄───────────────► │ SomnoTrace  │
│ SD card  │   HTTP /dir +  │  (GEEK)   │   metadata + file │             │
│          │    /download   │           │    streaming      │             │
└──────────┘                └───────────┘                   └──────┬──────┘
                                                                   │ home WiFi
                                                          uploads, dashboard,
                                                          MQTT, NTP — never drops
```

SomnoTrace stays on your LAN the whole time. The BLE link only carries data to one place.

---

## Hardware

SomnoBridge targets the **[Waveshare ESP32-S3-GEEK](https://www.waveshare.com/product/esp32-related/boards-kits/esp32-s3/esp32-s3-geek.htm)** — a USB-A stick form-factor board, ideal for hiding behind the nightstand powered from any USB port:

| Peripheral | Part | Notes |
|---|---|---|
| MCU | ESP32-S3 (dual-core LX7 @ 240 MHz) | 512 KB SRAM, **2 MB PSRAM**, **16 MB flash**, WiFi + BLE 5 |
| Display | 1.14" 240×135 IPS LCD (ST7789) | Status output: link state, RSSI, transfer progress |
| Storage | **TF card slot** | Used as a staging cache for files in flight |
| Power | USB-A male (plugs straight into a charger) | No battery — it's wall-powered infrastructure |
| Debug | 3-pin UART header, BOOT button | Serial console + reflashing |

**Requires:** any microSD card for the staging cache (small and slow is fine — it only needs to outpace BLE), and a USB power source.

SomnoTrace remains the **[Waveshare ESP32-S3-Touch-LCD-1.54](https://www.waveshare.com/esp32-s3-lcd-1.54.htm?sku=33869)** — it is the BLE client here.

---

## What the firmware does

1. **Provision** — on first boot (or after reset) SomnoBridge hosts its own WiFi AP + config portal (`somnobridge.local`). You set the EZShare SSID and password there.
2. **Join** — SomnoBridge joins the EZShare AP as a STA and keeps the association alive (auto-reconnect, RSSI monitoring).
3. **Serve** — it runs a BLE GATT server that SomnoTrace connects to, exposing a small **metadata API** plus a **file-transfer channel** that streams card contents over BLE.
4. **Report** — the LCD shows EZShare link state, BLE link state, RSSI, active transfer and throughput at a glance.

## Status display

The 1.14" LCD is a status panel — no UI controls:

- EZShare association, IP, RSSI, card reachability
- BLE link state and connected peer
- Per-transfer: filename, progress, effective BLE throughput
- Idle line: uptime, cache usage, last error (if any)

## BLE API (design target)

Two channels, one service:

- **Metadata / control characteristic** — small request/response commands (list directory, stat, file hash, device status, abort). JSON-encoded payloads.
- **Data characteristic** — file contents streamed as notifications: fixed-header frames carrying `file_id / offset / len / crc32`, pipelined and windowed so the sender never waits per-chunk. Ranged reads and resume are first-class — a dropped link must restart from an offset, not from byte zero.

Exact service/characteristic UUIDs and the command set land with the protocol spec (`spec/`) once the firmware skeleton exists.

## Getting files efficiently

BLE is the bottleneck, so the design minimizes its cost rather than papering over it:

- **BLE PHY**: 2 M PHY + Data Length Extension + negotiated 247-byte MTU, short connection interval — notifications stream, no request/response round trip per chunk.
- **Read-ahead**: while BLE drains the buffer, the WiFi side prefetches from the card over a keep-alive HTTP session into a ring buffer — BLE never stalls waiting on the card.
- **TF staging**: large transfers are spooled to SomnoBridge's SD card first, then served uninterrupted — the card's HTTP stack is slow and bursty, the BLE side wants a steady pipe.
- **Resume**: every frame carries its offset + CRC32, so reconnects resume instead of restarting.
- **No double buffering pain**: 2 MB PSRAM is the smooth-over-jitter buffer; the TF card is the real cache.

## EZShare client side

SomnoBridge speaks the card's observed HTTP protocol (documented in SomnoTrace's `spec/archive/files-over-http.md`):

- `GET /dir?dir=A:\path` — HTML `<pre>` directory listing (parsed, not scraped blindly)
- `GET /download?fname=…&fdir=…` — raw file bytes (8.3 short names)
- `GET /client?command=version` — card firmware version, used for the status page

On top of that, SomnoBridge keeps a small HTTP API of its own on the EZShare network for diagnostics and management (`/api/status`, `/api/cache`, `/api/config`) — useful from any laptop joined to the card's AP.

## Build & flash

ESP-IDF `v5.5.x`, target `esp32s3`. Firmware layout and build scripts will follow the SomnoTrace conventions (`scripts/idf.sh` wrapper, `build-dist.sh` for release images).

```
idf.py set-target esp32s3
idf.py build flash monitor
```

*(Repo skeleton lands alongside the first firmware commit.)*

## Licensing

GPLv3 with the same §7(b) attribution term as SomnoTrace — see `LICENSE` and `NOTICE`. Contributions require the SomnoTrace CLA.

## Documents

- [`docs/as10-time-drift.md`](docs/as10-time-drift.md) — problem statement: the
  AS10's clock drifts, EZShare has no usable clock, and everything SomnoBridge can
  learn is from passive filesystem observation. Maps the side channels worth
  investigating before designing a drift estimator.

- [`docs/proposal-final.md`](docs/proposal-final.md) — **current design** for
  measuring that drift: staged polling of the AS10's 60 s flush cadence, EDF
  header offset, NTP anchors over BLE, expected accuracy, software-only
  validation plan and the SomnoTrace hand-off.

- [`docs/proposal.md`](docs/proposal.md) — the original proposal and its five
  review rounds (§11, Appendices A–E). Kept as the review record; superseded by
  `proposal-final.md`.

## Related

- [SomnoTrace](https://github.com/ilyakruchinin/SomnoTrace) — the CPAP/oximetry recorder this bridges for
- EZShare protocol notes: `spec/archive/files-over-http.md` in the SomnoTrace repo
