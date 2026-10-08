# SomnoBridge — agent and contributor notes

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
./scripts/idf.sh build            # idf.py build inside espressif/idf:v5.5.5
./scripts/idf.sh set-target esp32s3
./scripts/idf.sh flash monitor    # IDF_PORT=/dev/ttyUSB0 to override default
./scripts/build-dist.sh           # merged image into dist/
```

ESP-IDF is pinned to `v5.5.5` (`IDF_TAG=` overrides).

**GEEK flashing quirk:** the board uses native USB-Serial/JTAG (`/dev/ttyACM0`).
After `flash`, esptool's RTS reset produces `USB_UART_CHIP_RESET`, which does
NOT re-sample strap pins — a chip that entered download mode stays in
`waiting for download` across every soft reset. Power-cycle (replug) or press
the physical RST button to boot the app. LCD pins: CS=10 MOSI=11 SCLK=12
DC=8 RST=9 BL=7; BOOT button = GPIO0.

**Never keep another process reading `/dev/ttyACM0` while flashing.** A
background `cat`/serial reader steals esptool's stub responses and the write
dies mid-stream with `Serial data stream stopped` / `Packet content transfer
stopped` — easy to misdiagnose as a bad cable or crash loop. Use bounded reads
(`timeout N cat ...`) and make sure nothing holds the port before `flash`.

**Board may stay in download mode after `flash`.** If the app doesn't come
up (no advertisement, serial prints `waiting for download`), run any esptool
op with `--after watchdog_reset`, e.g.
`esptool.py --chip esp32s3 -p PORT --before default_reset --after watchdog_reset chip_id`
— the watchdog reset resamples strap pins and boots the app. The port also
re-enumerates between `/dev/ttyACM0` and `/dev/ttyACM1` on every reset, so
tail whichever node is live (`/dev/serial/by-id/` is stable).

**The host's Intel AX200 can hard-wedge under sustained BLE notify load**
(stops scanning entirely; raw `hcitool lescan` → I/O error). Module reload,
USB rebind, `authorized` toggle, and PCIe FLR do not recover it — only a
host reboot. Have a fallback plan before running long download loops.

## BLE pairing / GATT testing (host side)

**Pairing:** `./scripts/pair_host.py` pairs this host in one uninterrupted
run — short-press the device button (`PAIRING 120S`), run the script, and
type the 6-digit LCD PIN at the terminal prompt (~25 s). `--code-file PATH`
polls a file instead (scripted use). Its BlueZ agent (KeyboardDisplay,
D-Bus) answers `RequestPasskey`; `Pair()` is issued ASYNC with a running
GLib loop — a synchronous `Pair()` blocks the loop, the agent never
answers, and the device cancels at its 30 s SMP timeout. Use `--re-pair`
when the device side lost the bond (reflash/NVS wipe/FORGET_PEER) —
BlueZ's stale record must be `RemoveDevice`d first, then the device
re-discovered (the Device1 object dies with the record).

`bluetoothctl` is unreliable for exercising the control characteristic — do
NOT trust it to tell you whether the firmware works:

- **`bluetoothctl`'s `write` command returns `org.bluez.Error.NotSupported`
  for every variant** (`<data>`, `<data> 0`, `<data> 0 request`, ...) even when
  the characteristic advertises `write`. It never reaches the device. Use the
  D-Bus API instead — this is what SomnoTrace does:

  ```bash
  gdbus call --system --dest org.bluez --object-path <CHAR_PATH> \
    --method org.bluez.GattCharacteristic1.WriteValue \
    "[0x01,0x01,0x03,0x00,0x01,0x00,...]" \
    "{'offset': <uint16 0>, 'type': <'request'>}"
  ```

- **BlueZ caches the GATT database for bonded peers** in
  `/var/lib/bluetooth/<adapter>/<device>/attributes` (separate from the bond
  keys in `info`). A cache written during an interrupted discovery can contain
  services only and a bogus `EndGroupHandle=65535`; BlueZ then silently rejects
  writes with `NotSupported` and never contacts the device. Delete the file
  (bond survives) and `systemctl restart bluetooth` to force rediscovery.

- `bluetoothctl` exits as soon as stdin closes, so `printf ... | bluetoothctl`
  never streams scan results — keep stdin open (coproc + `sleep`).

- Pairing requires the passkey be handed to SM via `ble_sm_inject_io()`;
  writing `ev->passkey.params.numcmp` is a no-op and the proc then stalls to
  the 30 s SMP timeout.

## Querying a live device (`scripts/ble_query.py`)

Host-side diagnostic for a bonded SomnoBridge (needs `python3-dbus` +
`python3-gi`; the device must be bonded already):

```bash
./scripts/ble_query.py                      # status + catalog + days + drift
./scripts/ble_query.py --status --catalog
./scripts/ble_query.py --days 14 --day 20240103
./scripts/ble_query.py --ls 'A:\'           # live card listing
./scripts/ble_query.py --drift              # live estimate + SOT envelope
./scripts/ble_query.py --time-sync 5        # run+commit a §8.1 anchor
./scripts/ble_query.py --evidence           # replay the evidence journal
./scripts/ble_query.py --get 'A:\DATALOG\YYYYMMDD\file.edf' --out FILE
                                            # download w/ SHA-256 verify
./scripts/ble_query.py --json               # raw JSON
```

It speaks the §10.1 framing over the D-Bus GATT API and reassembles
fragments itself. `--time-sync` performs N request/reply exchanges and
commits the lowest-RTT one (device ack shows `accepted`/`reason`/anchors),
which is the fastest way to confirm the drift machinery end to end.
Measured RTT includes host D-Bus/Python overhead, so treat it as an
upper bound — on-device processing shows up separately.

**`GET_FILE` may return error `STALE` for the first request touching a
catalog day whose listing is >90 s old.** The device schedules a rescan
(~10 s) and refuses rather than serving stale metadata — retry the same
op after ~12 s (`--get` already does this automatically; other clients
must too). Never treat `STALE` as a hard failure.

**Internal-RAM fragmentation can silently kill NimBLE.** The
`nimble_host` task (~12 KiB internal stack), MSYS pools, and SM pairing
crypto (`esp-aes` needs a multi-KiB contiguous block) all allocate from
internal RAM — `xTaskCreatePinnedToCore` failure is silent, leaving a
device that polls the card fine but never advertises (`sync=0`,
advertise rc=30). The 10 s `diag` log line reports
`adv/susp/sync/stage/htask/conn/bonded/heap_i/lrg_i/stream`; use it
instead of chasing early-boot serial output across the USB
re-enumeration gap. Keep BLE allocations early in `app_main` and budget
internal RAM conservatively.

## Timing characterization

```bash
python3 scripts/validate_timing.py self-test
python3 scripts/validate_timing.py history --day YYYYMMDD
python3 scripts/validate_timing.py capture --day YYYYMMDD --duration 1200
python3 scripts/validate_timing.py observe --day YYYYMMDD --duration 7200 --no-anchor-commit
python3 scripts/validate_timing.py analyze --input /tmp/somnobridge-validation-XXXX/evidence.jsonl
```

Evidence defaults to a private temporary directory with a mode-0600 JSONL
file. Only timing/layout metadata is retained; patient fields and waveforms
are not saved. Capture uses cached BLE catalog reads, existing serial poll
logs, and read-only SomnoTrace GetDateTime RPCs; it does not add card polls
or start/stop therapy. It owns the serial reader: stop capture before flashing.
The current LIST_DAY `scan_age_us` field actually contains the device's
last scan-start monotonic timestamp, not an age. The recorder accounts for
this legacy behavior. Host time-source uncertainty comes from chrony's root
distance; above 100 ms the anchor is best-effort/provisional rather than
qualified, and above 500 ms it is rejected. Exchange timings remain available
for offline analysis either way. These observations
alone do not establish a high-precision write-latency bound.

## Host tests

Pure-logic modules compile with plain `cc` against stub ESP headers:

```bash
./scripts/run_host_tests.sh    # builds + runs scripts/host_test.c
```

Stubs live in `scripts/test_include/` (`esp_err.h`, `esp_timer.h` with a
`host_now_us` virtual clock). Host catalog allocation uses plain malloc;
`run_host_tests.sh` enables `CAT_HOST_THREADS` with `-pthread` to use a real
mutex and exercise concurrent catalog rebuild/feed/read stress. Without
that define, catalog locks remain no-ops for single-threaded host use.

For address/undefined-behavior/leak checks, compile the same sources and
includes as the runner with `-O1 -DCAT_HOST_THREADS -pthread`,
`-fsanitize=address,undefined`, and `-fno-omit-frame-pointer`, then run with
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1`. Repeated catalog reset must remain leak-free.

`python3 scripts/validate_timing.py self-test` also checks journal replay
completion, byte-count/SHA-256 validation, and malformed-record rejection.
`python3 scripts/ble_query.py --self-test` checks BLE replay timeout/abort
and stale-frame handling without connecting to a device; it needs the same
`python3-dbus` and `python3-gi` imports as the query tool.

Target stack audit: `./scripts/idf.sh -D SOMNOBRIDGE_STACK_AUDIT=ON build`
produces GCC `.su` files alongside component objects. These report local
frames, not complete task call chains; ROM/newlib frames and callback
nesting still need live high-water measurements. The 10-second diagnostics
now include `stack task=... free_min=...` in ESP-IDF byte units. Exercise
real HTTP transfers, compressed streams, evidence, drift/status requests,
and abort/reconnect paths; synthetic streams alone do not cover HTTP.
Disable the audit flag with `-D SOMNOBRIDGE_STACK_AUDIT=OFF` if desired.

The host suite does not compile `components/scheduler/sched.c`. For queue
changes, also check target stack frames: the card task has a 16 KiB stack,
and nested planner/submit frames count together. Keep queue snapshots off
the task stack and pass the remaining destination capacity to drain helpers.
Heap probes are compiled only when heap poisoning is enabled; repeat live
BLE time-sync checks with poisoning disabled before accepting a crash fix.

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
