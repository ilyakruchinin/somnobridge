# SomnoPal — agent and contributor notes

WiFi↔BLE bridge firmware for the Waveshare ESP32-S3-GEEK, bridging the
EZShare WiFi SD card (in an AirSense 10) to SomnoTrace over BLE.
Maintained/architected by Ilya Kruchinin (@ilyakruchinin).

## Golden rules

- **Never commit.** The maintainer always commits themselves.
- **Clean-room implementation.** Do not copy SomnoTrace source code, assets,
  or headers. Protocol specs may inform behavior only (see
  `docs/implementation-spec.md` §0.2 / §15).
- **The spec is the contract:** `docs/implementation-spec.md` takes
  precedence over README and the other docs where they disagree.
- Health data: never put real patient data in tests, fixtures, or issues.

## Build (Docker is the only host dependency)

```bash
./scripts/idf.sh build            # idf.py build inside espressif/idf:v5.5.1
./scripts/idf.sh set-target esp32s3
./scripts/idf.sh flash monitor    # IDF_PORT=/dev/ttyUSB0 to override default
./scripts/build-dist.sh           # merged image into dist/
```

ESP-IDF is pinned to `v5.5.1` (`IDF_TAG=` overrides).

**GEEK flashing quirk:** the board uses native USB-Serial/JTAG (`/dev/ttyACM0`).
After `flash`, esptool's RTS reset produces `USB_UART_CHIP_RESET`, which does
NOT re-sample strap pins — a chip that entered download mode stays in
`waiting for download` across every soft reset. Power-cycle (replug) or press
the physical RST button to boot the app. LCD pins: CS=10 MOSI=11 SCLK=12
DC=8 RST=9 BL=7; BOOT button = GPIO0.

## Host tests

Pure-logic modules compile with plain `cc` against stub ESP headers:

```bash
./scripts/run_host_tests.sh    # builds + runs scripts/host_test.c
```

Stubs live in `scripts/test_include/` (`esp_err.h`, `esp_timer.h` with a
`host_now_us` virtual clock). Modules guarded by `ESP_PLATFORM` ifdefs use
plain malloc/no-op locks on the host.

## Layout

- `main/` — thin entry point / mode machine
- `components/` — first-party ESP-IDF components (one directory per module):
  `board` (LCD/button), `settings` (encrypted NVS), `netw` (STA/AP+portal),
  `ezshare` (HTTP adapter + host-testable parser/path), `catalog` (volatile
  DATALOG index), `scheduler` (card worker + therapy FSM), `ble_api`
  (NimBLE GATT + framing + ops), `drift` (anchors + fit + §8.5 record),
  `journal` (bounded NVS evidence ring)
- `docs/` — specifications; `implementation-spec.md` is authoritative
- `scripts/` — docker wrapper (`idf.sh`), host tests (`run_host_tests.sh`,
  `host_test.c`, `test_include/`)

## Licensing

GPLv3 + §7(b) attribution term — see `LICENSE` and `NOTICE`. This project is
separate from SomnoTrace; do not add "Based on SomnoTrace" notices.
