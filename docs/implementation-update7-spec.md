# SomnoBridge — implementation update 7: BLE throughput (Option 1, pure BLE)

Status: implemented, built on ESP-IDF v5.5.5, flashed, and partially
verified on-device. This document is the wire/config contract for the
throughput work recommended in `docs/speed-optim.md` Appendix D, Option 1.
The base implementation spec and updates 1–6 remain authoritative for
safety, privacy, authorization, and resource limits — none of those rules
change. SomnoTrace is untouched; see §10.6 for the compatibility rule that
keeps an unmodified v1 client working.

## 1. ESP-IDF version

SomnoBridge moves from `espressif/idf:v5.5.1` to `espressif/idf:v5.5.5`
(`scripts/idf.sh` default `IDF_TAG`). Rationale: S3 BLE connection fixes,
NimBLE memory options, the off-channel ESP-NOW fixes needed if Appendix D
Option 2 is ever tried, and routine patch-level fixes. SomnoTrace stays on
v5.5.1 — nothing in this update requires a matching version on the peer.

Note: v5.5.5's toolchain promotes `strncpy`/`snprintf` truncation patterns
to errors (`-Werror=stringop-truncation` / `-Werror=format-truncation`).
Fixed-size field copies now use `copy_bounded()` (memchr+memcpy, always
NUL-terminating, same truncation semantics).

## 2. Resource configuration changes

`sdkconfig.defaults` now sets:

- `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240` — CPU at 240 MHz. The deflate
  codec and frame packing are CPU-bound at stream rates; 160 MHz was
  marginal.
- `CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING` — NimBLE logged every GATT event
  at INFO; at stream rates that is thousands of UART lines per minute.
- `CONFIG_ESP32_WIFI_TASK_PINNED_TO_CORE_1` — WiFi driver task off core 0
  so it does not contend with the NimBLE host task.
- `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU1` — lwIP likewise on core 1.
- `CONFIG_TCP_SND_BUF_DEFAULT=16384`, `CONFIG_TCP_WND_DEFAULT=16384` —
  larger TCP windows for EZShare HTTP throughput; buffers are
  PSRAM-backed (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP` unchanged).
- `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL` — **required on v5.5.5**:
  the upgraded NimBLE's internal-RAM footprint left <10 KiB free
  (`heap_i≈8 KiB`), under which `nimble_mem_malloc` **asserts** —
  connection setup logged `BLE_INIT: Malloc failed` and the link died
  mid-stream. External mode moves NimBLE dynamic allocations to PSRAM;
  internal free RAM recovered to ~37 KiB. HCI/DMA transport buffers are
  allocated separately and stay internal.
- `MSYS_1_BLOCK_COUNT` raised 32 → 192 and `MSYS_2_BLOCK_COUNT`
  12 → 32 (both PSRAM-resident). Every notification pins ~3 pool blocks
  (data chain + ATT cmd + splits) until the controller drains it, so
  in-flight frames pin pool blocks; a credit burst bigger than the pool
  starves *control* indicates — observed as `STREAM_END` being dropped
  after ~5 s of `no mbuf` retries mid-stream. The stream credit window
  was also reduced to 48 × 240 B (~11.25 KiB, ~48 frames ≈ ~150 blocks
  in flight) so the pool always covers a full window plus ctrl
  headroom.

## 3. Wire protocol (§10.1 amendment): compact stream frames

A new frame kind `BLEP_STREAM_DATA = 10` carries bulk stream payloads on
the Data characteristic with a 4-byte header:

```
u8 ver(=1) | u8 kind(=10) | u16 seq | payload[N]
```

`seq` counts frames mod 65536 and lets the receiver detect a dropped
notification. There is no per-frame offset or request_id: streams are
exclusive on the link and bounded by rid-keyed STREAM_BEGIN/STREAM_END
control indications, so byte offsets are implied by in-order delivery.

Receivers MUST drop frames whose seq is *behind* the next-expected
counter (backward distance ≥ 0x8000), not just detect gaps forward:
when a second D-Bus client holds a notify session on the same
characteristic, BlueZ emits one `PropertiesChanged` per session, so a
concurrent poller makes the streaming client see every frame twice —
measured `rx=17480` for `dev=8740` sent, hci dump confirmed each ACL
packet arrived once. Backward-seq frames are duplicates by
construction (the sender never reuses `s_seq`); dropping them yields a
clean stream (`seq_err=0`, SHA-256 verified) even under concurrent
subscription. The v1 `FILE_BYTES` path is immune by accident — its
offset-indexed writes make duplicates idempotent. `STREAM_END` gains
`frames` (device-sent notify count) for sent-vs-received arbitration.

The same double-delivery hits *control* response fragments. A
duplicated tail fragment arriving after a message completed used to
reopen a fresh reassembly buffer, see `off > 0`, poison the rid, and
overwrite the finished response with `fragment_lost`. Receivers MUST
ignore `K_JSON_RESP` fragments for a rid whose response already
completed, unless the fragment is a genuine restart (`off==0` with
`FLAG_START`). Complementing that, the device sends `STREAM_END` twice
(~300 ms apart): a first copy damaged in flight is healed by the second
copy's `FLAG_START`, and a clean first copy makes the reassembled
duplicate harmless (it lands in an unread slot).

The 16-byte `blep_hdr_t` remains unchanged for JSON request/response,
ACK, ABORT, TIME_*, and the legacy `FILE_BYTES`/`EVIDENCE_BYTES` stream
kinds.

## 4. Credit semantics (§10.3 clarification)

ACK `{ offset, window }` is now interpreted strictly as a right-edge
grant: the sender may transmit stream bytes while
`sent + charge <= offset + window`. `sent` counts stream bytes on the
wire (for v2, block bytes including their 12-byte envelopes — the
receiver's ACK offset is the same wire-byte count). Stale ACKs can only
widen the edge via `max()`; they can never shrink it.

This is arithmetically equivalent to the old `acked`/`window` model for
v1 clients (their offset is contiguous received bytes and the window is
the grant beyond it).

## 5. Block codec (new §10.5)

v2 streams carry self-describing blocks, each independently decodable:

```
u8 codec | u8 ver(=1) | u16 rsv | u32 raw_len | u32 enc_len | enc[enc_len]
codec: 0 = raw passthrough | 1 = deflate(raw) | 2 = deflate(delta16(raw))
```

- Blocks are at most 64 KiB of raw input. The encoder stages input,
  tries `deflate(delta16)` first and `deflate(raw)` second, and emits
  whichever representation is smallest — falling back to a raw block
  when compression loses.
- `delta16` is a little-endian u16 difference transform (`d[i]=v[i]-v[i-1]`
  mod 2^16); on EDF-like sample data it roughly doubles deflate's gain.
- Deflate is the ESP32-S3 ROM miniz (`tdefl_init`/`tdefl_compress_buffer`,
  greedy parsing, 32 probes). The ~300 KiB compressor state lives in
  PSRAM: `tdefl_compress_mem_to_mem` allocates it from the default heap
  and would always fail under the internal-RAM budget. The low-level
  API performs no heap allocation itself.
- Staging/working buffers (2×64 KiB + 2×72 KiB + compressor state) are
  PSRAM, allocated per stream and freed at end. If any allocation fails
  the codec degrades to raw blocks — the wire format stays valid.
- Deflate backoff: after 8 consecutive blocks that lose to raw, deflate
  is retried only every 8th block — incompressible regions would
  otherwise burn ~300 ms/block of CPU for zero gain (measured: encode
  time on random data halved, ratio on compressible data unchanged).
- SHA-256 in STREAM_END is over **raw** bytes (unchanged from §10.2);
  the receiver verifies after decoding.
- STREAM_END gains `encoded_bytes` (wire bytes) alongside
  `actual_bytes` (raw bytes), plus `encode_ms` and a `blocks` counter
  histogram (`raw`/`def`/`d16`).

## 6. Negotiation (new §10.6)

The v2 wire (compact frames + block codec) is **opt-in per request**:
the client adds `"v2": true` to the GET_FILE or GET_EVIDENCE args.
STREAM_BEGIN reports the negotiated `"wire": 1|2`. Without the flag the
stream is the legacy v1 contract — 16-byte `FILE_BYTES`/`EVIDENCE_BYTES`
frames at raw byte offsets — so an unmodified SomnoTrace keeps working
unchanged.

SomnoTrace gets v2 in a later task; its client change is a codec decode
plus the `v2` flag, both gated on the negotiated `wire` field.

## 7. Sender pacing and packing

- Wire bytes accumulate in a residue buffer so every notification is a
  full ATT payload (`mtu-3-4` bytes); a short frame is emitted only for
  the true tail.
- Credit wait polls at 2 ms (it also feeds the ~500 ms auth-flap gate);
  a granted edge wakes the loop immediately.
- Send-side `ble_svc_send_bytes` failures (mbuf pool drain) retry with a
  1 ms yield rather than a fixed 10 ms sleep, so a freed buffer is
  refilled inside the same connection event when the controller allows.
- The post-download verification rescan is event-driven: the JOB_DAY
  completion callback notifies the stream task instead of a fixed
  2.5 s settle. A 10 s bound covers scheduler congestion.

## 8. PREPARE_DAYS (new op, §10.2)

`{"op":"PREPARE_DAYS","args":{"days":["YYYYMMDD",...]}}` queues a
`required` JOB_DAY refresh for each named day and replies with the
queued list. Batch clients call it once before a multi-file pull so the
per-file STALE/retry round trips disappear. Days already fresh (<90 s
resident scan) are reported as `skipped`.

## 9. STREAM_TEST (new op, transport benchmark)

`{"op":"STREAM_TEST","args":{"bytes":N,"pattern":"ramp"|"rand","v2":bool}}`
streams N synthetic bytes through the exact codec/framing/credit path —
no card access, no therapy gate interaction, safe during active therapy.
`ramp` produces 16-bit ramp samples (compressible; exercises delta16),
`rand` produces PRNG bytes (incompressible; exercises raw blocks and the
deflate backoff). Exists purely for transport measurement — SomnoTrace
does not need to implement it.

## 10. Card-agnostic path surface (§10.2 amendment)

Client-supplied `directory` arguments in `GET_FILE` and
`LIST_DIRECTORY` are interpreted as **card-root-relative logical
paths**: `/DATALOG/20261005`, `\DATALOG\20261005` and
`DATALOG/20261005` all resolve identically; `/` is the card root. An
explicit drive prefix is still honoured but only `A:`/`a:` passes
validation. Normalization happens once at the op boundary
(`ezp_logical_to_canon` → unchanged `ezp_canon_dir`); the canonical
`A:\…` form remains internal and feeds the card-facing URL builders
(`dir=A%3A%5C…`, `fdir=..\X\Y`) unchanged.

Response `directory` echoes (`LIST_DIRECTORY`, `STREAM_BEGIN`) emit the
logical `/…` form via `ezp_canon_to_logical`. Catalog-originated
responses never carried paths: `LIST_DAY`/`LIST_RECENT_DAYS` return
`day` + `name` tuples only. The rule exists so a future non-EZShare
card adapter changes the driver layer, not the wire contract.

## 11. EZShare HTTP keep-alive

`ezs_download` reuses a single `esp_http_client` handle across files:
the card honours keep-alive, so per-file TCP+request setup (~0.2–0.4 s
each) drops out of batch pulls. The handle is torn down on any error or
non-200 response, after 60 s idle, and is never reused while a response
body is undrained. All other EZShare paths (listings) are unchanged.

## 12. Journal self-heal (robustness fix)

On this unit the encrypted `evidence` NVS partition reached a state
where reads scanned fine but every `nvs_set_blob` failed with
`ESP_ERR_NVS_INVALID_STATE` — the journal was permanently dead.
`jrn_commit` now performs a one-shot heal: erase the `evidence`
partition, re-init, reopen, mark `truncated`, and retry the chunk once.
The journal is a bounded forensic ring that already self-documents
truncation; losing its history is the documented degraded mode.

## 13. Live-download fixes (real `GET_FILE`)

Synthetic `STREAM_TEST` validated the transport but never exercised the
EZShare fetch path. The first real-file week batch found two more
defects:

1. **`ble_stream` stack overflow.** Real `GET_FILE` runs
   `ezs_download()` + `esp_http_client` + the codec on the stream task;
   the 12 KiB stack (sized by the synthetic path, ~8.9 KiB peak)
   overflowed at `fetch start` and panicked. The task is back to 16 KiB
   as originally documented.
2. **NimBLE `npl_freertos_eventq_remove` assert** (`ret == pdPASS`,
   `npl_os_freertos.c:460`) — panic after ~20 min of real streaming.
   `ble_tx_task` called `ble_gatts_indicate_custom()` on a foreign task;
   the indicate path reschedules the HS callout, whose
   `callout_reset → eventq_remove` remove-and-requeue loop races the
   host task's blocking `xQueueReceive` on `ble_hs_evq` — the local
   `portMUX` provides no cross-core exclusion, so the
   `uxQueueMessagesWaiting` count can go stale mid-loop.
   `ble_svc_control_indicate()` now detects non-host-task callers
   (`s_host_task` captured in `host_task`) and defers through
   `nimble_port_get_dflt_eventq()` + `ble_npl_eventq_put`, running the
   indicate on the host task itself with a bounded synchronous wait.
   GAP/access-callback callers already on the host task keep the direct
   path. Data `notify` carries no ATT proc and stays direct.
3. **`BUSY` is a normal inter-stream state.** The stream task stays
   `active` through teardown (~300 ms redundant-END flush), so a
   back-to-back `GET_FILE` can see `BUSY`; clients retry it like
   `STALE` (~1.5 s). Documented in `ble-api.md`; `ble_query.py` retries
   it.

## 14. What is NOT in this update

- No ESP-NOW, no channel switching, no card config writes (Option 2
  from Appendix D is untouched).
- No SomnoTrace changes — the v2 wire is dormant until its client asks
  for it.
- No PHY/interval parameter changes on the peripheral side beyond what
  already shipped (the central, not us, controls conn params).

## 15. Verification

- Host suite: `scripts/run_host_tests.sh` passes (framing, codec
  round-trips, journal self-heal stubs added).
- `ble_query.py --self-test` passes (compact frames, block decode,
  legacy interop).
- On-device: v2 evidence stream negotiated (`wire:2`), blocks decoded,
  SHA-256 over raw verified; legacy evidence stream (`wire:1`) returns
  identical bytes unmodified; 2M PHY + 15 ms interval confirmed live.
- STREAM_TEST on-device (2 MiB each, BlueZ host, SHA-256 verified,
  0 sequence errors):

  | pattern | wire bytes | time | wire rate | raw-effective |
  |---------|-----------|------|-----------|----------------|
  | ramp v2 |   448,535 | 8.4 s | 52.4 KiB/s | **235 KiB/s** |
  | rand v2 | 2,097,548 | 22.3 s | 91.7 KiB/s | 91.7 KiB/s |
  | rand v1 | 2,097,152 | 20.8 s | 98.5 KiB/s | 98.5 KiB/s |

- Worst case reproduced and fixed: a second host process holding
  `StartNotify` on the data characteristic makes BlueZ double-deliver
  every notification *and* every control-fragment indication. With the
  session active: rx_frames=17480, dup_frames=8740 dropped by
  backward-seq dedup, seq_errors=0, SHA-256 matched, STREAM_END
  delivered, ~92–99 KiB/s wire — identical wire integrity to a clean
  run. btmon capture proves the adapter received each ACL packet once
  (8740 notifications at HCI level = dev_frames); the duplicate was
  purely a host-side delivery artifact, not a device resend. The
  dup-tail-fragment self-poison on completed rids (see §3) was found
  and fixed the same way: `fragment_lost` no longer fires under a
  concurrent subscriber, and the redundant `STREAM_END` resend covers
  genuine in-flight fragment loss.

  The legacy v1 path roughly doubled the pre-change baseline (~20–50
  KiB/s → ~106 KiB/s) purely through packing/credit/pacing fixes; v2 +
  codec lifts compressible payloads to ~237 KiB/s restored.
  Host-side measurement note: this laptop's BlueZ occasionally drops a
  notification under sustained load (HCI RX overrun — documented
  AX200 hazard in AGENTS.md); `seq` counters make the loss visible and
  the host tool now reports it instead of silently corrupting decode.
- The initial week run collected 218 files / 16.48 MiB, but mixed builds
  and reused verified downloads across reflashes. Only its final 72 files
  were fetched on the final build from that run. Its driver checked SHA
  but did not enforce all completion/length checks; this is not a clean
  full-week certification of the subsequent audit fixes.

## 16. Follow-up stack, lifecycle and throughput audit

The audit retained internal EZShare paths and the POSIX wire surface.
Additional fixes include owned/serialized deferred-indication buffers,
retaining stream ownership until cancellation cleanup completes, bounded
credit waits and ACK validation, explicit initial-credit errors with buffer
cleanup, checked task creation, successful-generation-specific post-check
callbacks, preserved callbacks during scheduler merges, and serialized queue
snapshot/requeue operations. Monitoring freshness now ages OUT back to UNKNOWN.

The 4 KiB HTTP chunk moved to PSRAM: GCC's download frame fell from 5,360
bytes to 1,648 bytes before later instrumentation. Keep the 16 KiB task
allocation; local frame sizes are not complete call-chain bounds. Inbound
cJSON nesting is capped at 16, outbound ATT payload sizing at MTU 247.
Observed compressed real-file stream high-water headroom improved to about
5.8 KiB; these measurements do not prove every task/path cannot overflow.

Empty cached days now return an empty list rather than relying on malloc(0).
Live root listing uses EZShare's native `dir=A:` spelling while echoing `/`;
`dir=A:\\` returned malformed HTML on this card. Both root listing and an
empty-day response passed live checks. The generic URL formatter also lacked
`=` before its second query value, producing `&fdir..…` and a 404 for the
native download fallback. Formatting now has host regression coverage,
complete percent-encoding buffers, and explicit truncation failure. The
corrected native download form still needs a quiet-card live comparison.

The host retains completed-response state after consuming a result, filters
D-Bus signals by characteristic path, unregisters its receiver on close,
waits for a redundant END after fragment poisoning, and ignores duplicate
BEGIN messages during collection. Legacy holes are rejected rather than
zero-filled; odd delta16 tails work in the optional NumPy path. Files are
saved only after transport, source/activity, decoded/encoded counts, exact
Content-Length where available, and SHA checks all pass. Legacy evidence
uses kind 9; file END flags mark EOF rather than every HTTP chunk.

Measured on the same 607,226-byte file (252,008 encoded bytes):

| condition | HTTP read waiting | downstream sink | device SHA updates | host SHA |
|-----------|-------------------|-----------------|--------------------|----------|
| compressed real GET_FILE | 43.327 s | 6.650 s | 0.164 s | 0.0013 s |
| modem sleep disabled | 40.920 s | 7.111 s | 0.152 s | 0.0012 s |
| read/discard, BLE connected | 45.005 s | 0.176 s | 0.169 s | n/a |
| read/discard, BLE disconnected | 40.001 s | 0.170 s | 0.161 s | n/a |

SHA is included in sink time; compression and BLE pacing are also included
for a real transfer. Modem-sleep disabling did not demonstrate a material
improvement and was reverted. All isolated reads matched the real-transfer
SHA.

**Root cause resolved.** The slow reads were caused by
`CONFIG_LWIP_TCP_RECVMBOX_SIZE=6` against a 16,384-byte TCP receive window
and 1,440-byte MSS: the mailbox could hold ~6 in-flight segments while the
window advertised ~11, so the connection spent most of its time recovering
from mailbox overflow. Espressif recommends roughly `TCP_WND / MSS + 2`
entries. Raising the mailbox to 16 produced, on the same file:

| condition | HTTP read waiting | total |
|-----------|-------------------|-------|
| recvmbox=6, form 0/1 | 43.5 s / 41.8 s | 45.4 s / 43.4 s |
| recvmbox=16, form 0/1, BLE connected | 2.42 s / 1.84 s | 4.0 s / 3.5 s |
| recvmbox=16, BLE disconnected | 1.29 s | n/a |
| recvmbox=16, real v2 GET_FILE | 1.03 s | 7.3 s end-to-end |

The residual connected-versus-disconnected gap is WiFi/BLE radio
coexistence, not a further defect. With the card path fixed, BLE credit
pacing is the remaining limiter (~80–90 KiB/s raw for compressible data,
~25 KiB/s for incompressible v1 streams). SHA-256 was never a factor
(~0.13–0.16 s device, ~1.5 ms host for 607 KiB).

`SOMNOBRIDGE_HTTP_AUDIT=ON` enables a temporary authenticated read/discard
`HTTP_TEST` diagnostic. It deliberately denies file-completion permission,
can finish a diagnostic read after the test client disconnects, and must be
OFF in the final image. Normal firmware exposes no such operation.

Final-image validation (audit OFF, recvmbox=16) passed: deep-JSON
rejection, duplicate-day preparation, POSIX root listing, three
abort/recovery cycles, explicit no-credit timeout, 20 back-to-back
verified transfers, an odd-length 262,145-byte stream, and concurrent
GET_STATUS during a 607 KiB transfer. A fresh seven-day download verified
218/218 listed files (16.48 MiB raw, 6.36 MiB wire, ~44 KiB/s raw
aggregate including per-file overheads, ~8.3 min wall) with zero
sequence/decode errors, zero panics or stack overflows, and continuous
5-second card polling (462 HTTP service calls, zero failures).
