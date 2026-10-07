# SomnoBridge BLE API

Client-facing reference for the GATT service SomnoBridge exposes to
SomnoTrace. `docs/implementation-spec.md` §10 is the contract; this
document describes what is implemented, including the update7 stream
extensions. A working reference client is `scripts/ble_query.py`.

## 1. Service topology

Advertised device name: `SomnoBridge`; advertisements carry the primary
service UUID.

| Object | UUID | Properties |
|---|---|---|
| Service | `8ba7d100-6e4b-4bb4-9f1d-7306766e6f50` | primary |
| **Control** characteristic | `8ba7d101-6e4b-4bb4-9f1d-7306766e6f50` | write, write-without-response, indicate, read-encrypted |
| **Data** characteristic | `8ba7d102-6e4b-4bb4-9f1d-7306766e6f50` | notify |

- **Control**: client → device JSON requests (write or write-no-rsp),
  device → client JSON responses (indicate). ACK credits and ABORT also
  travel on this characteristic.
- **Data**: stream payload notifications only (device → client).
- Negotiate ATT MTU 247; connection targets 2M PHY + DLE. The peer picks
  the actual PHY/interval.

## 2. Security and connection rules

- Exactly **one bonded peer** (SomnoTrace). LE Secure Connections +
  bonding + MITM passkey entry; the six-digit passkey is shown on the
  device LCD. No Just Works.
- A new central can pair only inside a **120 s pairing window** opened
  by a physical short press on the BOOT button while unbonded (up to
  60 s per attempt, ≤3 failures).
- All accesses are gated: an unauthenticated link gets nothing. A
  bonded reconnect must complete encryption before ops or indications
  are served. Responses are **indications** — the client must subscribe
  to the Control CCCD (and Data CCCD before streaming).
- Unbonded clients are disconnected outside the pairing window.

## 3. Frame format

All multibyte fields little-endian. Control characteristic payloads are
fragmented to the ATT payload size with a 16-byte header:

```
u8 ver(=1) | u8 kind | u16 flags | u32 request_id | u64 offset
| payload…
```

`flags`: `0x0001` START (first fragment), `0x0002` END (last). A
single-fragment message carries both. `offset` is the byte offset of
the fragment within the logical message (JSON body, stream byte, or —
for ACK — the contiguous receive offset).

Kinds:

| kind | name | direction | purpose |
|---:|---|---|---|
| 1 | `JSON_REQUEST` | c→d | `{"op":"NAME","args":{…}}` |
| 2 | `JSON_RESPONSE` | d→c | `{"op":…,"ok":…,"result"/"error"}` |
| 3 | `FILE_BYTES` | d→c | v1 file stream data |
| 4 | `ACK` | c→d | stream credit grant |
| 5 | `ABORT` | c→d | cancel active stream |
| 6 | `TIME_REQUEST` | c→d | time-exchange round (§7) |
| 7 | `TIME_REPLY` | d→c | exchange reply |
| 8 | `TIME_COMMIT` | c→d | commit lowest-RTT round |
| 9 | `EVIDENCE_BYTES` | d→c | v1 evidence-journal stream |
| 10 | `STREAM_DATA` | d→c | v2 compact stream frame (§5) |

JSON bodies ≤ 4096 bytes; fragment reassembly deadline 30 s. Numbers
that can exceed 32 bits are JSON **strings** (`"size":"607232"`, also
timestamps in µs) — parse accordingly.

Response envelope: `{"op":"<OP>","ok":true,"result":{…}}` or
`{"op":"<OP>","ok":false,"error":{"code":"<EC>"}}`.

## 4. Operations

| op | args | result (selected fields) | notes |
|---|---|---|---|
| `GET_STATUS` | — | `mode`, `sta_connected`, `rssi`, `paired`, `ble_connected`, `security`, `therapy{state,stale,quiet_since_ms,expected_append_ms,confidence,reason,transfer_allowed}`, `budget{active_8h,backoff_ms}`, `phase`, `http`, `loop` | therapy gate lives here |
| `GET_CATALOG_STATUS` | — | `state`(SCANNING/READY/INCOMPLETE/ERROR), `folders_total`, `catalog_complete`, `hot_ready`, `incomplete_count`, `hot_days[]` | catalog is volatile |
| `LIST_RECENT_DAYS` | `days:int` (≤36600, default 14) | `days[]` (YYYYMMDD, newest first), `catalog_complete`, `hot_ready` | catalog view, no card hit |
| `LIST_DAY` | `day:"YYYYMMDD"` | `day`, `folder_stale`, `scan_age_us`, `files[]{name,size,ts}` | `scan_age_us` is actually the scan-start monotonic timestamp (legacy) |
| `LIST_DIRECTORY` | `directory:"/…"` | `directory`, `files[]{name,size,ts}` | live card read; therapy-gated; refuses `DATALOG` (use catalog ops) |
| `GET_FILE` | `directory`, `name`, `expected_size`, `v2:bool`? | stream — see §5 | therapy-gated; begins a stream |
| `PREPARE_DAYS` | `days:["YYYYMMDD",…]` | `days[]{day,status:"fresh"/"queued"}`, `eta_ms`, `transfer_allowed` | pre-warms stale listings before a download batch |
| `GET_DRIFT` | `best_available`/`include_provisional:bool`? | `schema`, `estimate`, `source_of_truth`, `qualified_source_of_truth`, `provisional_source_of_truth`, `completed_session_fallback`, `best_available_offset` (opt-in only) | `source_of_truth` is only a fresh qualified live candidate; persisted records are `historical, applicable:false` |
| `GET_EVIDENCE` | `v2:bool`? | stream — journal replay | `stream_type:"evidence"` |
| `STREAM_TEST` | `bytes` (≤32 MiB), `pattern:"ramp"/"rand"`, `v2:bool`? | stream — synthetic bulk | touches no card/NVS; usable during therapy |

Error codes: `BAD_REQUEST`, `UNSUPPORTED_OP`, `BUSY`, `NOT_FOUND`,
`NOT_SCANNED`, `SCANNING`, `CATALOG_INCOMPLETE`, `STALE`,
`THERAPY_ACTIVE`, `THERAPY_UNKNOWN`, `SETUP_ACTIVE`, `SIZE_MISMATCH`,
`SOURCE_CHANGED`, `PATH_INVALID`, `HTTP_ERROR`, `PARSE_ERROR`,
`TIMEOUT`, `RESOURCE_LIMIT`, `BUDGET_EXHAUSTED`, `ABORTED`,
`LINK_LOST`, `METADATA_UNAVAILABLE`, `STORAGE_ERROR`,
`SECURITY_NOT_PROVISIONED`.

`GET_FILE`/`LIST_DIRECTORY` are refused with `THERAPY_ACTIVE` or
`THERAPY_UNKNOWN` while the card may be appending — poll
`therapy.transfer_allowed` in `GET_STATUS`; `PREPARE_DAYS` +
a retry after `eta_ms` smooths the ~90 s listing-freshness guard.

`BUSY` is transient: the previous stream's task stays `active` for
a few hundred ms after `STREAM_END` (redundant-END flush + teardown).
Clients issuing back-to-back files MUST retry `BUSY` after a short
pause (~1.5 s), the same contract as `STALE`.

### Path syntax

`directory` arguments are card-agnostic, card-root-relative:

- `/DATALOG/20261005`, `\DATALOG\20261005`, `DATALOG/20261005`, `/` —
  all resolve to the card root namespace (canonical `A:\…` internally).
- Explicit drive letters are still accepted but only `A:`/`a:` pass;
  `B:\…` → `PATH_INVALID`. Prefer the bare `/` form.
- Both `/` and `\` separators work; `.`, `..`, empty components
  (`//`), `%`, control/FAT-invalid characters and `:` inside
  components are rejected with `PATH_INVALID`.
- Responses echo directories in the logical `/` form
  (`LIST_DIRECTORY.directory`, `STREAM_BEGIN.directory`). The `A:\`
  spelling is an EZShare-adapter detail that never crosses the wire in
  file listings — `LIST_DAY`/`LIST_RECENT_DAYS` carry only `day` +
  `name` tuples anyway.
- `name` is a single filename component (LFN or 8.3 alias); the device
  resolves the card's actual download name itself and reports it back
  in `STREAM_BEGIN.download_name`.

## 5. Streams

One stream at a time (`BUSY` otherwise). Lifecycle:

1. Client writes the op (`GET_FILE`, `GET_EVIDENCE`, `STREAM_TEST`).
2. Device indicates a `JSON_RESPONSE` whose op is `STREAM_BEGIN`:
   `stream_type` (`file`/`evidence`/`test`), `wire` (1 or 2), plus
   `listed_size`/`source_before{size,ts}` for files or
   `journal_seq`/`oldest_seq`/`truncated` for evidence.
3. Client streams **credits**: `ACK` frames (kind 4) on Control,
   header `offset` = contiguous wire bytes received, 4-byte LE
   `window` payload. Right-edge semantics — the sender may transmit
   while `sent < offset + window`; stale ACKs only widen the edge, so
   send them freely (write-without-response is accepted and preferred).
   Grant the first credit immediately after `STREAM_BEGIN` — the
   sender waits for it. Windows above 65,536 bytes and offsets beyond
   bytes sent are ignored; the effective outstanding window is capped
   at 11,520 bytes to preserve control-buffer headroom. A credit stall
   is bounded to 10 seconds.
4. Payload arrives on Data as notifications.
5. Device indicates `STREAM_END` (a `JSON_RESPONSE`): `ok`,
   `actual_bytes` (raw), `encoded_bytes`/`listed_size`/`content_length`,
   `sha256` (hex, over **raw** bytes), `frames` (notify count sent),
   `blocks{raw,def,d16}`, `stall_ms`, `retry_ms`, `encode_ms`, file-only
   `source_unchanged`/`therapy_status`/`completion_allowed`, `error`.
   **`STREAM_END` is emitted twice ~300 ms apart** — see §8.
   File diagnostics additionally report `sha_us`, `http_header_us`,
   `http_read_us`, `http_sink_us`, and `http_reads`. HTTP read time excludes
   the sink callback; sink time includes SHA, encoding and BLE pacing.
   `encode_ms` also includes the final partial-block flush, so the timing
   counters are not independent quantities to sum indiscriminately.

A file may be published only if both response/transport `ok` and
`completion_allowed` are true, `source_unchanged` and `monitoring_checked`
are true, decoded raw and encoded byte counts match the verdict, the exact
Content-Length matches when available, and SHA-256 matches with no sequence
or decode errors. The listed size may be rounded: use `size_unit_shift`,
not exact equality to `listed_size`. A matching hash alone does not prove
source immutability or successful post-checks.

`ABORT` (kind 5) with the stream's `request_id` cancels mid-flight.
Disconnect aborts the stream. Aborting requests cancellation; the stream
remains `BUSY` until its worker releases the old buffers/state. An initial
credit timeout is an explicit `STREAM_END` error, not a silent idle transition.

Inbound JSON parsing is limited to 16 nesting levels to bound parser stack
use. Outbound ATT payloads use at most MTU 247 even if a larger MTU is negotiated.

### Wire 1 (legacy, default)

Data frames are `FILE_BYTES`/`EVIDENCE_BYTES` with the full 16-byte
header; `offset` indexes raw bytes — duplicate or replayed frames are
idempotent (same offset, same bytes). `sha256` still covers raw bytes.
`GET_EVIDENCE` v1 uses `EVIDENCE_BYTES`.

### Wire 2 (opt-in `"v2":true`)

Data frames use a 4-byte header: `u8 ver(=1) | u8 kind(=10) |
u16 seq | payload`. `seq` counts frames mod 65536; byte offsets are
implied by in-order delivery. The payload is a sequence of
self-describing blocks:

```
u8 codec | u8 ver(=1) | u16 rsv | u32 raw_len | u32 enc_len | enc[…]
```

`codec`: `0` raw passthrough · `1` deflate(raw) ·
`2` deflate(delta16(raw)) — delta16 maps LE uint16 samples to
`x[i]-x[i-1]` mod 2^16 (odd trailing byte passthrough). Blocks ≤ 64 KiB
raw; a zero-length stream emits one empty block. Decode: inflate, then
inverse-delta for codec 2. SHA-256 verifies the reassembled **raw**
stream, not wire bytes; `encoded_bytes` is what credits measured.

## 6. Timing exchange

`TIME_REQUEST` payload: `t1:u64` (client UTC µs) | `unc:u32` (client
clock uncertainty µs) | `epoch:u32`. `TIME_REPLY`: `t1:u64 | m2:u64 |
m3:u64 | epoch:u32` where `m2`/`m3` are device monotonic µs bracketing
the reply. The client stamps receipt (`t4`), runs several rounds, then
commits the lowest-RTT one via `TIME_COMMIT` (`t1:u64 | t4:u64 |
epoch:u32`). Device answers `{"accepted":bool,"reason","uncertainty_us",
"anchors"}`. Rejections: `rtt`, `uncertainty`, `stale`, `new_epoch`,
`discontinuity`, `bad`.

## 7. What a client MUST implement

- Subscribe to both CCCDs before requests; all responses are
  indications on Control, stream data is notifications on Data.
- Grant credits proactively (grant ≈25 % of the window at a time) and
  never rely on write responses for ACKs during a stream.
- **Dedup compact frames**: drop `seq` behind next-expected. BlueZ
  emits one `PropertiesChanged` per notify session — a concurrent
  poller on the same device makes every frame arrive twice (measured
  rx=17480 for dev=8740 at HCI level).
- **Ignore response fragments for a completed request id** unless
  `off==0 && FLAG_START` — a duplicated tail fragment must not reopen
  reassembly. Conversely, a fresh `FLAG_START` on a poisoned/lost
  rid clears the poison: treat it as a device resend (this is how the
  duplicated `STREAM_END` heals a damaged first copy). Retain completed-id
  state after consuming a response, not only while it is queued. Ignore
  duplicate `STREAM_BEGIN` messages during collection, and allow the
  redundant `STREAM_END` to repair a poisoned first copy before aborting.
- Verify `sha256` over raw bytes; compare `frames` vs unique-seq count
  to detect in-flight loss.
- Expect JSON numbers-as-strings for ≥32-bit quantities.

Measured throughput (2 MiB, 15 ms interval, 2M PHY): v1 ~98–106 KiB/s
wire; v2 ~92 KiB/s incompressible, ~52 KiB/s wire / ~235 KiB/s raw on
compressible payloads. Host-side BLE adapters under sustained notify
load (notably Intel AX200) can drop frames — sequence counters make
such loss visible rather than corrupt.

## 8. HTTP management

There is **no HTTP listener** in normal operation. Diagnostics/config
exist only inside SETUP mode's setup AP (`SomnoBridge-Setup-XXXX` →
`http://192.168.8.1`, session cookie + CSRF, button-confirmed
destructive actions). See `implementation-spec.md` §4.1.
