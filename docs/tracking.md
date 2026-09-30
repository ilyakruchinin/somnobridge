# DATALOG Metadata Tracking and File Synchronization Requirements

**Status:** design, pre-implementation. `proposal-final.md` remains the clock/therapy
design; this document defines the DATALOG metadata catalog and synchronization
ownership between SomnoPal and SomnoTrace. Terminology follows `proposal-final.md`
(`therapy_active`, `transfer_allowed`, `stale`, fitted `P`, hot-window listing).

## 1. Goals and non-goals

**Goals**

- SomnoPal maintains a **volatile, rebuildable, DATALOG-only metadata catalog** in
  PSRAM, fed by the same EZShare listings the timing controller already issues.
- SomnoTrace is the **sole persistent sync authority**: it compares requested
  recent catalog metadata against its own durable state and decides what to fetch.
- Simple, fetch-on-demand APIs; no versioning protocol.

**Non-goals**

- No TF card requirement for the catalog. No file-content snapshots. No persistent
  transfer database on SomnoPal. **No `boot_id`, generation counters, per-entry
  versions, or change-log protocol.**
- Files outside `A:\DATALOG` are **not** indexed or cached. SomnoTrace accesses
  them through the live, uncached `LIST_DIRECTORY` and generic `GET_FILE` proxy
  APIs (§5) only when its own policy says so (e.g. STR / root-summary refresh
  after DATALOG changes — that policy belongs to SomnoTrace).

## 2. Noon-day model and hot folders

- DATALOG child folders are named `YYYYMMDD` and sort lexically =
  chronologically. Malformed names are rejected for date-range selection but
  exposed as diagnostics.
- **Nominal noon-day** in the configured local timezone: before local noon the
  *base day* is the previous calendar date; at/after noon it is the current date.
- **Hot-eligible names:** `base−1`, `base`, `base+1` (deduplicated) — tolerates
  timezone/clock discrepancies and noon rollover.
- Never infer child-file changes from folder timestamps: FAT parent `mtime` may
  not change on append.
- **Poll strategy:**
  - Startup: hot folders first.
  - While no active folder is identified and early detection is needed, **or around
    noon rollover**: list all existing hot candidates ~once per minute, **and** poll
    the DATALOG parent ~once per minute — creation of a previously nonexistent hot
    folder must not wait on an unspecified parent interval.
  - Once coherent growth identifies the active folder: list **that** folder every
    fitted therapy cycle (~minute, per `proposal-final.md` §5.5); adjacent hot
    folders may be slower (~5 min) except around noon rollover.
  - Outside rollover, once the active folder has locked, the parent may slow to
    ~5 minutes. A new matching hot folder found in the parent listing is promoted
    immediately.
  - At therapy stop: a final refresh of active/hot folders.
- This preserves the early-timing behaviour of `proposal-final.md` without
  permanently doing 3 listings/min — the multi-folder watch exists only until an
  active folder locks.

## 3. Startup and all-folder crawl

- Sequence: list DATALOG parent → build the known day set → scan hot folders
  first → crawl every other valid day folder **newest-first** at a bounded
  background rate.
- Full inventory is **not** immediate. API exposes a simple catalog state:
  `SCANNING | READY | ERROR`, plus `folders_total`, `folders_scanned`,
  `hot_ready`, `oldest_scanned_day`. No boot IDs or generations.
- Recent/hot requests may succeed once `hot_ready` with `catalog_complete=false`;
  whole-card requests require `READY` or explicitly indicate partial.
- The catalog is lost on reboot and rebuilt — safe, because SomnoTrace remains
  authoritative.
- A full folder refresh **atomically replaces** that folder's in-memory entry
  set, so deletions/renames leave no stale entries.

## 4. Per-entry data — minimal fields

- `day` (stored once per folder), exact exposed `name` / download identifier,
  `size`, and a **nullable** `timestamp_raw` holding exactly what the listing
  supplied. **No** `last_seen`, version, or generation fields.
- Comparison tuple: `(day, name, size, timestamp_raw)`. `size` is primary for
  append-only sampled EDFs. `timestamp_raw` is required in the schema but may be
  null/unavailable — do not assume EZShare always supplies a usable timestamp.
- Timestamp semantics are unverified: preserve the raw value, never manufacture
  precision.
- The exact EZShare-exposed names/aliases must be used for download. Alias
  stability must be tested; never infer a timestamp from hashed 8.3 names.

## 5. API (SomnoPal, over BLE)

Keep it simple — per-day granularity avoids pagination-consistency and version
schemes.

- `GET_CATALOG_STATUS` → `SCANNING|READY|ERROR`, `folders_total`,
  `folders_scanned`, `hot_ready`, `oldest_scanned_day`, `catalog_complete`.
- `LIST_RECENT_DAYS { days: N }` → selected valid day names covering the past `N`
  nominal noon-days **plus all hot names that exist**, deduplicated, with
  `catalog_complete` / `hot_ready`.
- `LIST_DAY { day }` → the complete current cached entry set for one scanned
  folder. Returns `NOT_SCANNED`/`SCANNING` rather than silently partial. The
  response includes the folder scan time for diagnostics only — it is not
  comparison identity.
- `LIST_DIRECTORY { directory }` → performs a **live, uncached** EZShare listing
  for a canonical, validated directory outside `A:\DATALOG`. It is a read-only
  passthrough for root/SETTINGS/other files that SomnoTrace chooses to refresh;
  it never inserts results into the DATALOG catalog. It is allowed only when
  `transfer_allowed == true`, except an explicit diagnostic override.
- `GET_FILE { directory, name, expected_size? }` → generic live EZShare download.
  For a catalogued DATALOG file, `directory` is derived from its noon-day and the
  response declares `cached_size_at_start`; for an uncached file, SomnoTrace
  supplies the validated directory and `cached_size_at_start` is `null`. The
  response always declares actual bytes/content length. When `expected_size` is
  supplied, a known pre-body mismatch rejects the request. No claim of HTTP Range.
- Both live proxy APIs must canonicalize paths, reject traversal and write-like
  operations, and restrict access to the EZShare volume rooted at `A:\`.
- SomnoPal **never** decides which files are "needed" — it lacks SomnoTrace
  state. It returns requested metadata; SomnoTrace decides.
- BLE framing/chunking is a transport detail. One logical `LIST_DAY` response
  must be internally consistent: copy the entry descriptors under the catalog
  lock, then send; never hold the catalog lock across BLE I/O.

## 6. SomnoTrace algorithm and state

- Setting `recent_noon_days` = `N` — recommended initial **14**, configurable.
  This is a **query window**, not SomnoPal retention.
- Sync loop: query status → `LIST_RECENT_DAYS(N)` → `LIST_DAY` each returned day
  → compare remote `(day, name, size, timestamp_raw)` to persistent local state.
- Persistent state minimum: `day`/`name`, last downloaded size and raw timestamp,
  local completion marker. No remote generation.
- Download to `.part`; atomically publish only when the body length matches the
  declared transfer length.
- A later catalog size/timestamp difference returns the file to
  `NEEDS_DOWNLOAD`. **Never mark a path permanently complete.**
- Rescan the recent `N` days each sync. Older tracked files need no regular
  query; a full/manual rescan can list older cached days.
- Root/STR/etc. are not catalog entries. After detecting a relevant DATALOG
  change, SomnoTrace uses `LIST_DIRECTORY` to obtain their current live metadata,
  compares it with its own state, and fetches selected files with generic
  `GET_FILE`. These results remain uncached on SomnoPal.

## 7. Toilet-break / resume race — honest, simple rules

- The catalog is minute-fresh, **not a lock**. If therapy resumes during a `GET`,
  SomnoPal may detect it only at the next listing — or only after a long GET if
  EZShare cannot serve a listing concurrently.
- **Before each GET:** SomnoTrace requires `therapy_active == false`
  (OUT_OF_THERAPY) and `transfer_allowed == true`. For a DATALOG file, its cached
  folder must be fresh (non-`stale`) and its tuple must match `expected_size`.
  For an uncached file, SomnoTrace must first obtain fresh metadata from
  `LIST_DIRECTORY`, then pass that size as `expected_size`.
- **After each GET:** SomnoPal prioritizes an immediate refresh of **both** (a)
  the source directory, to re-check the file tuple — cached folder refresh for
  DATALOG, live `LIST_DIRECTORY` otherwise — and (b) the active/hot DATALOG
  folder needed for therapy detection, if different. SomnoTrace waits for both
  required results before marking the file complete. If the source tuple changed,
  therapy is active/unknown, any required refresh failed/is `stale`, or the body
  length differs: keep or discard `.part`, and **do not** mark complete.
- A resume immediately after the post-check can still grow the same path; for
  append-visible files or detectable timestamp changes the next recent-days
  comparison catches the difference and re-downloads, so the race degrades to an
  **extra download**. The remaining blind spot is a same-size rewrite whose
  listing timestamp also does not change — fundamentally invisible to listing
  comparison and covered only by the conservative type/session re-fetch policy
  (§8).
- Fetch **smallest files first, large BRP last** to shrink the detection blind
  interval. No snapshot/staging required.
- If Range is later verified it may improve interruptibility, but it is not
  required.

## 8. Edge cases and requirements

- **Noon rollover / timezone-date mismatch:** covered by the three hot folders
  (`base−1`, `base`, `base+1`).
- Multiple sessions/files in the same day.
- Folder gaps, malformed names, non-date folders.
- Empty folders; deleted/renamed files (atomic folder replacement handles these).
- **Same-size rewrite:** a changed timestamp detects it; if the EZShare timestamp
  does not update, listing alone cannot detect it. Conservative policy: SomnoTrace
  may re-fetch known rewrite-prone DATALOG types after a new session; validate
  actual AS10 behaviour.
- 8.3 alias changes/collisions appear to SomnoTrace as delete+create: download
  under the new exposed name, but **never auto-delete** the old local data.
- Catch-up append with a size jump spanning multiple records.
- HTTP/listing errors: preserve the prior cache but mark the folder `stale`;
  never present it as fresh/complete — fail closed for transfer.
- Reboot mid-scan or mid-API request.
- **Memory exhaustion:** compact folder-owned names/entries; a configured catalog
  memory ceiling; **never silently omit** — report `ERROR`/`INCOMPLETE`, and cold
  folders may stay `NOT_SCANNED`/evicted only with explicit status. Rough
  non-guarantee: thousands of compact records likely fit in ~2 MB PSRAM; measure
  on a real card.
- **Card replacement:** there may be no reliable card identity. Clear and rebuild
  the catalog on a wholesale parent-listing discontinuity, an explicit
  card-change/user action, or another verified identity signal if E0 discovers
  one. An identical-looking replacement rebuilds only on restart or manual
  refresh — do not claim guaranteed automatic detection. Never auto-delete
  SomnoTrace data.
- Sizes must be **uint64** in the protocol even though files are likely small.

## 9. Acceptance criteria

- Boot crawl priority: hot folders scanned before the background newest-first
  crawl.
- API partial/ready semantics: `hot_ready` gating, `catalog_complete` flag,
  `NOT_SCANNED`/`SCANNING` instead of silent partials.
- `LIST_DIRECTORY` returns live outside-DATALOG metadata without inserting it into
  the catalog; generic `GET_FILE` validates paths and works for both catalogued
  DATALOG and freshly listed uncached files.
- Hot promotion on parent-listing discovery; noon rollover across `base±1`; the
  ~1/min parent poll (no active folder or rollover) discovers a newly created hot
  folder within ~1 minute.
- Growth reflected in catalog within one fitted cycle of the active folder.
- Folder refresh atomically replaces entries; deletions/renames leave no stale
  rows.
- Catalog rebuild after reboot; rebuild on card replacement without touching
  SomnoTrace data — detection only via the §8 triggers (parent discontinuity,
  explicit action, verified identity); an identical-looking swap is not claimed
  to be detected automatically.
- Stale/error listings fail closed: not presented as fresh, `transfer_allowed`
  stays false.
- SomnoTrace detects a changed `size` or `timestamp_raw` and returns the file to
  `NEEDS_DOWNLOAD`; a previously completed path is never treated as permanently
  complete.
- Download race: post-GET refresh covers both the source folder tuple and the
  active/hot folder; any change, failure, or staleness prevents completion, and a
  detectable race degrades to re-download (not omission) — the same-size/no-
  timestamp blind spot remains documented.
- Same-size limitation documented and covered by the conservative re-fetch
  policy.
- Memory ceiling reached → explicit `ERROR`/`INCOMPLETE`, no silent omission.
- No API or stored state carries `boot_id`, generation, or per-entry versions.

## 10. Rationale

This is deliberately simple: a volatile PSRAM mirror of EZShare listings, an
unversioned compare tuple, and per-day APIs. There is no arbitrary 14-day cache
retention on SomnoPal — all metadata is retained while PSRAM permits; `N`
controls only SomnoTrace's query window. Persistent truth remains SomnoTrace's
local state plus the EZShare card as source of record, so reboots, card swaps,
and races are recoverable by rebuild/compare and degrade to re-downloads rather
than data loss — **except** the explicitly documented blind spot: a same-size
rewrite whose listing timestamp does not change, which only the conservative
type/session re-fetch policy can cover.
