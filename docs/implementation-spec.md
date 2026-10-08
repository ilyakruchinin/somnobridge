# SomnoBridge Firmware Implementation Specification

**Status:** implementation contract, version 1. Hardware-dependent timing features require the validation gates in §14; their accuracy is not a demonstrated guarantee.

**Purpose:** a small, secure WiFi-to-BLE bridge that observes an AirSense 10 through an EZShare card, estimates clock drift, and keeps a rebuildable file-metadata catalog for SomnoTrace. No firmware implementation or SomnoTrace changes are included in this document.

## 0. Authority, source documents, and scope

The source documents have different jobs:

- [as10-time-drift.md](as10-time-drift.md): defines the clock problem, passive-observation constraints, and candidate evidence channels. Its experiments are investigation ideas, not permission for routine in-therapy downloads.
- [proposal-final.md](proposal-final.md): designs the timing model, staged acquisition, therapy detection, evidence collection, and downstream hand-off. Its numeric predictions remain hypotheses until validated.
- [tracking.md](tracking.md): defines the volatile DATALOG catalog and assigns persistent synchronization decisions to SomnoTrace.

For implementation, **this document takes precedence where these documents or the README disagree**. The original documents remain unchanged as rationale and research history. Requirements marked MUST are acceptance conditions; SHOULD permits a documented reason to depart. Hardware characterization must not silently change this contract: unsupported capabilities yield degraded metadata or an explicit error.

### 0.1 Conflict resolutions

| Topic | Version 1 decision |
|---|---|
| Single-folder watch versus three hot folders | Watch all existing hot folders and the DATALOG parent approximately once/minute. Existing-folder detection is approximately 60 seconds, not a guaranteed 15 seconds. Parent discovery schedules an immediate child scan. |
| Catalog polling versus timing load | One scheduler and one request counter cover every EZShare request. Parent, neighbor, startup, retries, and post-download listings are not free. Cold crawling pauses during therapy. |
| 0.2-second refinement versus a 1 request/s peak | Validated dense acquisition may reach 5 listing starts/s for short bounded bursts; sustained adaptive refinement permits guarded 500 ms-or-slower spacing, and ordinary requests permit at most 1/s. Per-window/peak limits remain binding; no cumulative active-session listing cap applies (§6). |
| `SCANNING/READY/ERROR` versus `INCOMPLETE` | Four explicit catalog states; folder completeness and freshness are separate (§5). |
| Missing status/drift API | `GET_STATUS` and `GET_DRIFT` are defined in §10. |
| Therapy `stale` versus folder `stale` | Use `therapy_stale` and `folder_stale`, with explicit age limits. |
| Client-only versus server transfer gating | SomnoBridge enforces all live file/list requests; client checks are additional protection. Cached metadata remains queryable during therapy. |
| Historical versus active file transfers | No ordinary live file download, including historical files, while therapy is active, unknown, or stale. |
| Mandatory TF staging/resume in README | No TF requirement, file snapshots, or persistent transfer database. Stream through a bounded RAM ring. Version 1 restarts interrupted files from byte zero. |
| First observed append versus first session append | Resolve record/flush indices from verified header and size evidence; otherwise publish pending/ambiguous drift, never silently shift by one cycle. |
| Physical cadence versus logical EDF duration | Fit physical cadence separately. Derive a sample-time scale only after verifying the record-to-flush relationship. |
| Precision estimates versus error bounds | Publish model-qualified intervals and uncertainty components; do not label unvalidated assumptions as measured accuracy (§8). |
| NTP path | SomnoTrace supplies authenticated BLE time exchanges. SomnoBridge does not join a home WiFi network or trust the card's HTTP clock. |
| Open HTTP management on EZShare WiFi | No HTTP listener in normal STA operation. Setup/diagnostics are confined to a physically opened, short-lived open AP (§4.1). |

### 0.2 Deliberate exclusions

- No wired AS10 connection, UART calibration, service-port cable, or writes/configuration calls to EZShare.
- No content cache, TF staging, catalog boot IDs, generation counters, per-entry versions, delta protocol, or SomnoBridge decision about which files SomnoTrace needs.
- No automatic deletion of SomnoTrace data, inference of filenames from hashed aliases, or guarantee of detecting identical-looking card swaps.
- No routine annotation tail reads or incremental downloads during therapy.
- No network OTA or arbitrary HTTP proxy in version 1. Future OTA requires a separate authenticated, signed-update contract.

SomnoBridge is a separate firmware, not a SomnoTrace fork. Protocol documentation may inform behavior; do not copy SomnoTrace source, assets, UI, build scripts, or license headers. This document makes no legal assertion about licensing obligations; source provenance and licensing must be checked before firmware dependencies are added.

## 1. Architecture and ownership

```
AS10 + EZShare -- WiFi STA / read-only HTTP --> SomnoBridge
                                                 |
                                    paired, encrypted BLE
                                                 |
                                             SomnoTrace
                                    home WiFi / NTP / storage
```

SomnoBridge owns:

1. EZShare connection and bounded HTTP requests.
2. A PSRAM catalog of DATALOG day folders and their file metadata.
3. Monotonic listing observations, therapy state, and timing estimates.
4. A small persistent evidence/metadata journal, not a download ledger.
5. Secure BLE APIs and live, validated file streaming.
6. NVS settings, pairing bonds, button handling, and LCD status.

SomnoTrace owns:

1. Its NTP clock and BLE central/passkey-entry UI.
2. Persistent comparison tuples and atomic local downloads.
3. Recent-day query policy (initial recommendation: 14 days).
4. Root/STR/SETTINGS refresh policy after relevant DATALOG changes.
5. Durable drift metadata, cross-night RTC-rate estimation, and corrections to EDFs it generates.

Catalog, therapy detection, and timing use **the same parsed listing observation**. Neither subsystem independently polls the card. BLE disconnection does not stop observation; missing time anchors lowers timing quality, not catalog availability.

## 2. Hardware and execution baseline

Target: Waveshare **ESP32-S3-GEEK**, ESP32-S3R2, 16 MiB flash, 2 MiB PSRAM, 1.14-inch ST7789 240×135 LCD, USB power, and BOOT button. WiFi and BLE share one antenna. The TF slot is unused by version 1.

Use ESP-IDF C/FreeRTOS with NimBLE; pin the initial toolchain to **ESP-IDF v5.5.1**, then review security updates before a production release. Use IDF's HTTP, WiFi, NVS, cryptographic, and LCD facilities rather than importing another firmware's framework. No new external component is required by this spec.

| Function | GEEK GPIO |
|---|---:|
| BOOT, active-low, pull-up | 0 |
| LCD CS | 10 |
| LCD MOSI | 11 |
| LCD SCLK | 12 |
| LCD DC | 8 |
| LCD reset | 9 |
| LCD backlight, active-high | 7 |

Confirm the physical board revision against the manufacturer schematic at bring-up. Do not substitute the SomnoTrace board's pin map. Verify LCD rotation, visible-area offsets, color order, and backlight before pairing can be enabled: an unreadable PIN is a security/commissioning failure.

**BOOT caveat:** GPIO0 is a boot-strapping pin. Holding it while powering/resetting may enter ROM download mode rather than firmware setup. All setup instructions MUST say: power the device normally, wait for its status screen, then hold BOOT. Firmware must not fight the boot strap or claim a power-on hold reliably enters its AP.

### 2.1 Button behavior

Debounce for 30 ms. Classify a press on release; execute at most one action per press.

- Short press, 30 ms–<1 second: wake/cycle status pages; when unbonded in normal operation, also open a 120-second BLE pairing window.
- Hold, ≥3 seconds: display `Release for setup`; on release enter SETUP mode. No credentials or bonds are erased.
- Presses from 1–<3 seconds have no action.
- In SETUP, short press confirms a pending destructive configuration action shown on the LCD; otherwise cycles the setup instructions. A ≥3-second hold exits setup without saving pending edits.

Resetting settings, forgetting a bond, and clearing evidence each require a portal request followed by a **separate** short physical press within 30 seconds. Show the exact action before accepting confirmation. Holding BOOT alone never factory-resets the device.

## 3. BLE security and trust boundary

### 3.1 Pairing and reconnecting

SomnoBridge supports **one bonded SomnoTrace identity and one BLE connection**.

- Require LE Secure Connections, bonding, MITM-authenticated passkey entry, and a 128-bit encryption key: Bluetooth security level 4. No legacy pairing or Just Works fallback.
- SomnoBridge advertises DisplayOnly I/O capability. SomnoTrace provides keyboard/passkey entry and supplies the displayed six-digit passkey through its BLE stack.
- Generate a fresh random passkey per attempt using the hardware-backed RNG, including leading zeros. Display it only on SomnoBridge's LCD; do not expose it in advertisements, serial logs, HTTP, or an unencrypted characteristic.
- An unknown central can pair only within the physically opened 120-second window (§2.1). Each attempt expires at the earlier of 60 seconds from its start or closure of that window. Close the window after success or three failures; another physical short press is required to retry.
- If a bond already exists, deny new pairing. Replacing SomnoTrace requires the physically confirmed `FORGET_PEER` setup action, which clears the old bond before a new window can open.
- Persist NimBLE bond material and the authorized peer identity in encrypted NVS. Do not authenticate by an advertised name or raw current MAC address; use the bonded identity/IRK so private addresses reconnect correctly.
- On reconnect, complete encryption and verify authenticated Secure Connections plus the authorized identity **before** permitting any application request or notification.
- Lost client keys do not trigger automatic bond replacement. An NVS/bond-write failure fails pairing closed; show a recoverable error rather than reporting successful enrollment.

Service discovery and a generic device name may be public. Control, data, time, status, metadata, and evidence accesses MUST enforce authorization, including CCCD subscription and notification delivery. Disconnect unbonded clients outside the pairing window and incomplete unauthenticated connections after 10 seconds, except an authorized in-progress passkey attempt uses its 60-second pairing deadline. Configure stack security permissions and also verify the connection security state at the application boundary.

### 3.2 Privacy and limits

Advertisements contain the service UUID and a generic device label, never therapy status, patient identifiers, files, SSIDs, PINs, or drift. Logs redact secrets and do not dump request bodies or EDF content. Store only necessary parsed header fields; never persist the EDF patient-ID field.

BLE pairing protects the SomnoBridge–SomnoTrace link. It does **not** make the EZShare's plaintext HTTP endpoint secure against another client on the card's WiFi, prove that a card contains a particular patient's data, or make a bonded peer's supplied NTP truthful. Protect EZShare WiFi with a strong password where its firmware permits it, and report open/weak WiFi as a warning. Do not silently join an open card AP: require an explicit setup acknowledgment.

At-rest production security is defined in §4.2. Physical reflashing/debug access is a separate threat boundary; do not describe a developer build as tamper-resistant.

## 4. Setup, settings, and normal WiFi operation

### 4.1 Modes and setup portal

Device mode is `SETUP | CONNECTING | RUNNING | FAULT`. Therapy state is independent. First boot without valid settings enters SETUP; an ordinary WiFi failure never opens an AP automatically.

Entering SETUP MUST:

1. Close the pairing window/cancel pending pairing, reject new live BLE work, and abort any in-flight transfer with an explicit error. An existing authorized bonded connection may remain for local status only.
2. Set therapy UNKNOWN/stale and record an observation gap.
3. Disconnect EZShare STA; do not operate an EZShare bridge while the portal is open.
4. Start an open AP named `SomnoBridge-Setup-XXXX` (`XXXX` random per session). **Deliberate deviation from the usual WPA2-PSK recommendation:** the AP password was dropped for UX — the LCD is too small to transcribe a 12-character password comfortably, and the residual protection it offered is low: a motivated attacker in radio range during a short-lived, physically initiated window could observe it anyway. Security instead relies on the physical gate (long-press to open), the short session bounds below, the single-client cap, CSRF/session hardening, and the button confirmation for destructive actions. The residual risk — an in-range attacker could push a different EZShare SSID during an open setup window — is accepted; it yields at most a wrong bridge endpoint, recoverable by re-entering setup.
5. Serve a local portal at `192.168.8.1`, with at most one WiFi client. Show the address explicitly; captive-portal detection is optional, never required for setup.

Close the AP after a successful save, a runtime exit hold, or five minutes without authenticated portal activity; enforce an absolute 15-minute session limit. With no valid settings, closing setup leaves a local `Setup required — hold BOOT` screen. Credentials and bonds remain intact on timeout. A new setup session gets new AP/session secrets.

The portal serves only local embedded assets, never external scripts/fonts. After joining the open AP, bootstrap a random session cookie and CSRF token from the landing page. Require them on configuration/diagnostic API requests; cookie is HttpOnly and SameSite=Strict. Accept only the local Host, reject cross-origin requests, and do not enable CORS. Mutations require POST with JSON, matching Origin and a CSRF header. No configuration in URL query strings. Cap HTTP request bodies at 4 KiB, clients at one, and idle sockets at 10 seconds. Rate-limit API requests to 5/s with a burst of 10.

The open AP carries plaintext HTTP — anyone in radio range during a setup window can observe or attempt portal traffic. This is a deliberately local, physically initiated commissioning channel; do not present it as an internet-accessible management interface, and never keep it open in normal operation.

| Portal endpoint | Contract |
|---|---|
| `GET /` | Local setup page; session bootstrap. No existing password rendered. |
| `GET /api/config` | Non-secret settings and `password_set`; never return the saved EZShare password or bond keys. |
| `POST /api/config` | Validate a complete candidate configuration, commit it atomically, reply `saved`, then close AP and connect. Omitted password preserves the old password; clearing requires explicit `clear_password:true` plus open-WiFi acknowledgment. |
| `GET /api/status` | Redacted device diagnostics, counters, memory, firmware/security profile, and last error. No patient files or raw evidence. |
| `POST /api/action` | `FORGET_PEER`, `RESET_SETTINGS`, `CLEAR_EVIDENCE`, or `REBUILD_CATALOG`; destructive actions await the separate button confirmation in §2.1. |

`RESET_SETTINGS` clears EZShare/timezone settings, card characterization, catalog, and timing journal, but preserves the BLE peer unless separately forgotten. `FORGET_PEER` disconnects BLE and clears both the authorized identity and all stack bond records. `CLEAR_EVIDENCE` does not alter EZShare or SomnoTrace. `REBUILD_CATALOG` clears only volatile metadata and schedules a rebuild after setup closes. Each action returns its specific scope before confirmation. Normal STA mode MUST have no portal, HTTP listener, or mDNS management advertisement.

### 4.2 NVS and at-rest storage

Persist settings as one versioned, validated NVS blob in namespace `sp_config`, key `settings`. Fields:

| Field | Type/default |
|---|---|
| `schema` | unsigned integer, `1` |
| `ezshare_ssid` | 1–32 bytes, valid UTF-8 for the portal; preserve exact configured bytes |
| `ezshare_password` | 8–63 ASCII characters, or 64 hexadecimal PSK digits; empty only with explicit open-network acknowledgment |
| `allow_open_wifi` | boolean, false |
| `ezshare_bssid` | optional selected MAC; null means no BSSID lock |
| `http_host_ipv4` | optional IPv4 in the associated STA subnet; null uses its DHCP gateway |
| `as10_tz` | validated POSIX TZ rule, ≤96 ASCII bytes, initial `UTC0`. The portal's machine selector maps ResMed 10-series to `UTC0` (no offset — the AS10 clock is set by hand) and ResMed 11-series to the machine's configured GMT offset |
| `timezone_confirmed` | boolean, false until user confirms this represents the AS10 clock |
| `lcd_brightness` | integer 1–100, initial 30 |

Version 1 has no arbitrary URL, proxy destination, poll-rate slider, or user switch that enables unverified dense polling. Catalog ceiling and polling safety limits are build constants. The recent-day query window belongs to SomnoTrace, not this blob.

Use NVS's power-loss-safe blob replacement and commit before reporting `saved`. A failed commit leaves the prior valid settings usable. Validate again on boot; unknown schema, corruption, or inconsistent bond state enters a visible recoverable FAULT, not silent NVS erase. The WiFi driver MUST use RAM credential storage; `sp_config` is the only persistent WiFi-settings authority. Keep stack bond data in its encrypted NVS store and reconcile it with the authorized identity before enabling the service.

**At-rest protection:** encrypt config, bonds, and the evidence NVS partition using ESP-IDF's ESP32-S3 HMAC-backed NVS encryption scheme with a unique protected per-device eFuse HMAC key when one is provisioned. Protect the key against readout. Firmware MUST check for a provisioned key and MUST NOT automatically burn an eFuse on first boot.

**Deliberate deviation (shipped default `CONFIG_SOMNOBRIDGE_DEV_UNPROTECTED=y`):** an unprovisioned board — no burned eFuse HMAC key — runs `DEV-UNPROTECTED`, storing settings, bond identity, and evidence in **plain NVS** rather than failing closed. Rationale: an `espefuse` provisioning step is a high barrier for from-source builders, and the residual exposure is modest — reading the plaintext requires physical flash access, and the EZShare link is itself plaintext HTTP. Boards provisioned with the eFuse key still take the encrypted path; the flag is only the no-key fallback. Report the state honestly (`ENCRYPTED` / `DEV-UNPROTECTED`) and set the flag `n` for a strict deployment profile, where missing protection yields `SECURITY_NOT_PROVISIONED`.

Secure Boot/flash encryption for resistance to malicious firmware replacement is a separate explicitly authorized deployment choice, not something to enable irreversibly during routine development. No keys or credentials belong in repository files, fixtures, release images, or logs.

Persist bonds only at enrollment/revocation and settings only at explicit saves. Do not persist catalog entries, periodic status, or a boot counter in settings NVS. Evidence is the separately bounded journal in §9.

### 4.3 EZShare STA connection

Use 2.4 GHz STA with the saved credentials and optional BSSID lock. Resolve the card only to the saved IPv4 or DHCP gateway within the directly associated subnet, TCP port 80. No internet DNS, external NTP, arbitrary hosts, or redirect following. Reject a multicast, broadcast, loopback, or off-subnet destination.

Reconnect with 1, 2, 4, 8, 16, 30, then 60-second delays, with ±10% randomized jitter; reset after 60 seconds of stable association. Association is not proof of card reachability. WiFi/HTTP failures make therapy stale/unknown and block live transfers; cached listings remain available with their age/error flags. Never treat disassociation as therapy ending.

On a new association, discard outstanding HTTP state, re-list DATALOG, and refresh hot folders before allowing transfer. Reset Range capability to unknown after a suspected card change, changed BSSID/version, changed credentials, or unverifiable replacement. No reliable automatic card identity is assumed.

## 5. Volatile DATALOG catalog

### 5.1 Data and selection

Catalog only immediate valid date-named child folders of `A:\DATALOG` and all regular files inside those folders. Do not recursively crawl other directories. A valid day is a real Gregorian `YYYYMMDD` date, not merely eight digits. Report malformed/unexpected folders as diagnostic counts, not fabricated days.

Each file entry has exactly `name`, uint64 `size`, and nullable `timestamp_raw`; day is stored once per folder. Preserve the exact download alias and original timestamp text, with no invented precision or per-entry last-seen/version field. Comparison identity is `(day, name, size, timestamp_raw)` only. Path casing is canonicalized separately from the exact download name (§11).

Folder control data is permitted: complete-entry-set flag, scan-in-progress, last successful monotonic scan time, last error, and stale flag. These fields are diagnostics/freshness, not synchronization identity.

When time and timezone are usable, compute the nominal noon-day from the configured AS10-local calendar: before noon use yesterday, otherwise today. Hot names are `base−1`, `base`, `base+1`, using calendar arithmetic rather than subtracting 24 hours across DST. They are candidates, not evidence that therapy exists.

Without usable time or a confirmed timezone, seed hot candidates from the newest valid card day and its adjacent calendar dates; report `base_source:card_latest` and `date_uncertain:true`. This supports rebuilding after reboot without trusting a filename as NTP. Replace this fallback when a trustworthy BLE anchor/configuration becomes available. A growing folder stays monitored even if it falls outside the nominal hot set; report `hot_window_mismatch` and require explicit timezone/card-clock correction if necessary.

Never use directory mtimes to infer child updates. Discovery of a new day folder alone does not prove therapy; its contents must be scanned.

### 5.2 Startup and refresh

1. List DATALOG parent; retain the complete known valid day-name set.
2. Scan existing hot candidates first. A missing candidate is not an empty successful listing unless absence was established by a successful parent listing.
3. Start serving complete scanned days immediately, with partial-catalog flags.
4. Once quiet therapy state is established, crawl unscanned older days newest-first, at no more than one folder/5 seconds, with all other work taking priority.
5. Pause cold crawling during IN_THERAPY, uncertain resume detection, acquisition, and file transfers. It may consequently take several idle periods to finish a large card.

Parse a full listing into bounded scratch storage before replacing a folder's entries under a short lock. Preserve the old entries if HTTP, parse, size, or allocation checks fail, and set `folder_stale:true`. Successful replacement removes vanished/renamed remote rows, never SomnoTrace's local files. New parent results reconcile the folder-name set similarly.

| Catalog state | Meaning |
|---|---|
| `SCANNING` | Parent known and crawl/retry work remains; some complete days may be served. |
| `READY` | Every known valid day has a complete resident entry set. Does not imply that every cold folder is recently scanned. |
| `INCOMPLETE` | A resource limit prevents full residency; omitted/unscanned days are explicitly identified. |
| `ERROR` | Parent discovery or another essential catalog operation failed; last usable data may remain. |

`catalog_complete` is true only in READY with every known day resident and no unresolved listing error. `hot_ready` means all currently existing hot candidates have complete resident entry sets and their existence/absence is known. It is not a substitute for the freshness checks in §7/§12.

Reserve at most **768 KiB PSRAM** for resident catalog entries. Preserve hot/active folders first. If capacity is exceeded, leave cold days explicitly NOT_SCANNED or evict the oldest cold entry sets, set INCOMPLETE, and expose affected days. Never return a truncated day as complete. A single oversized hot/day listing returns `RESOURCE_LIMIT` with no successful partial result; timing may also degrade. Do not reset the device or delete user data to recover memory.

Copy a complete day's compact descriptors into a bounded response view (≤128 KiB) under the lock; serialize fragments after releasing it. If the view cannot fit, return `RESOURCE_LIMIT`; do not add pagination/version machinery silently. Whole-card enumeration is `LIST_RECENT_DAYS` with a sufficiently large N followed by per-day reads; absent days are simply absent, but unscanned known days are explicitly reported.

Rebuild on reboot, confirmed card-change action, verified identity change if available, or wholesale parent-list discontinuity. Identical-looking card replacement and same-size rewrites with unchanged timestamps cannot be guaranteed detectable. A `REBUILD_CATALOG` action and recent-day comparisons are the recovery path, not auto-deletion.

## 6. One EZShare scheduler and measured load control

### 6.1 Ownership, priority, and global rules

One worker owns all card HTTP operations, with **at most one in-flight request**. A listing observation records request-start/response-end monotonic times, the complete parsed result, and the purpose. Feed catalog, therapy FSM, estimator, and journal from that observation. No other task can bypass the worker.

Priority, highest first: transfer post-validation and resume checks; therapy liveness/idle confirmation; time-sensitive acquisition/gates; hot-folder discovery; pre-transfer validation; cold crawl. Deduplicate simultaneous refresh requests for the same folder. Do not perform card requests in BLE callbacks or while holding catalog locks.

- Count every HTTP attempt, including failures, under its real purpose for diagnostics. There is **no cumulative active-session listing cap**, including no 700-listing/eight-hour allowance. Sustained observation MUST NOT be suspended or coarsened solely because a total request count or elapsed session duration was reached.
- Watch, active timing/liveness, recovery, and verified-idle bulk work have separate diagnostic counters. All requests share the one-worker, spacing, and backoff rules; counters are telemetry, not permission tokens.
- Ordinary non-refinement requests: minimum 1 second between request starts. Sustained adaptive refinement (§6.3): minimum 500 ms, widened to at least twice the measured service median. Validated dense acquisition: minimum 200 ms. All modes prohibit concurrency and catch-up bursts of missed polls.
- At most **40 acquisition listings** per newly observed file set apply to the legacy short dense-acquisition ladder, including its coarse, ladder, and reacquisition attempts in the first 30 minutes. This allowance does not apply to sustained 500 ms-or-slower refinement or ordinary liveness/recovery. Across all file sets, retain at most **40 dense-mode listing starts in any rolling 60 seconds**; multi-folder dense acquisition cannot multiply that peak allowance.
- Give required liveness priority over optional work when actual service delay, worker contention, or backoff prevents timely scheduling. If required monitoring becomes stale, set UNKNOWN/stale and block transfer rather than asserting OUT. Do not manufacture `budget_exhausted` merely from cumulative request counts.
- No listing polls while a file body is being streamed. Post-checks run before any subsequent live request. No assumption of listing/download concurrency.
- Exponential HTTP backoff: 2, 4, 8, 16, 30, then 60 seconds; reset after three successful normal-service listings. Request failures do not count as successful quiet evidence.
- Service-time baseline is the median of the last 32 successful non-dense listings in the current association, excluding failed/oversized responses. Until eight samples exist, use 1 second. Service time above 3× baseline triggers backoff and disables fine acquisition for that association. Record the actual delay in uncertainty; no fixed 100 ms blur assumption.

Request-count telemetry stays in bounded RAM; do not checkpoint a cumulative request-budget ledger to NVS. Existing `load_budget` data is obsolete and MUST NOT gate monitoring on boot, missing/corrupt history, checkpoint failures, or a lost-tail reserve. Leave unrelated encrypted NVS settings, bonds, timing evidence, and completed-session fallback persistence unchanged. Do not automatically erase legacy keys or partitions.

On recovery, resume observation through the normal UNKNOWN state, fresh listing baselines, request spacing, response-time guards, and backoff. No historical-count reconstruction or eight-hour waiting period is required. A reboot must not fabricate quiet evidence or bypass therapy freshness checks. Preserve short-term dense-mode peak limits separately from the removed cumulative ledger.

### 6.2 Watch, hot discovery, and cold work

Watch scans each existing hot folder and the parent approximately once every 60 seconds, staggered across the minute (up to four requests/minute). Do this throughout the day; version 1 has no separate bedtime window or 15-second detection promise. A newly discovered hot folder gets a priority scan within 5 seconds when service/backoff permits. A new file in an existing hot folder is normally discovered within one minute; a new folder normally within about 65 seconds. These are healthy-service targets, not guaranteed under errors, service delays, or backoff.

Once an active folder is identified, its timing/liveness schedule replaces its watch scan. Outside noon rollover, scan the parent and each other hot folder **once/hour**, staggered. This adds at most three/hour rather than the unbudgeted 5-minute scans in tracking.md. Preserve any separately observed growing folder as active too; do not silently monitor only one if several grow. Extra required liveness can reduce estimator precision when requests contend for the worker; stale or incomplete monitoring must fail closed explicitly, without a cumulative count-based suspension.

Rollover window is ±5 local minutes around noon. Scan parent and neighboring hot candidates once/minute in that window, subject to the same one-worker, spacing, service-time, and backoff rules. Promote newly discovered folders immediately. Preserve the previous active folder until it independently stops growing; changing calendar date does not end therapy or finalize a session.

Cold crawling occurs only during confirmed idle periods, at ≤1 folder/5 seconds. API reads of cached metadata do not trigger crawling. Stop/idle confirmation refreshes all relevant hot/active folders as §7 requires, not just the downloaded source.

### 6.3 Timing acquisition and tracking

Sustained adaptive refinement uses a bounded cluster around the predicted visible update throughout therapy, as specified by `implmenetation-update6-spec.md` §§5 and 13: nominal 500 ms request-start spacing, measured service guard, at least three planned starts when prediction coverage permits, at most ten per window, cancellation after a usable transition, and explicit miss/reacquisition fallback. Recompute the prediction from recent intervals so the cluster can move earlier or later. Do not automatically coarsen to 1-second MAINTAIN solely because acquisition ended or the session has lasted a certain time. Preserve actual request/response bounds and diagnostic load counters. This scoped best-effort path retains `timing_unvalidated` and does not qualify the timing source.

Faster dense timing profiles (including the legacy 200 ms ladder below) MUST be enabled only by a build-time characterization record that passed §14 for the relevant AS10/EZShare combination. It includes supported card firmware, verified listing semantics, safe polling rate, and record/flush model. Unknown profiles must not enable that 200 ms ladder or claim characterized sub-second accuracy; the sustained best-effort path above remains available with its explicit assumptions and guards.

For a validated profile:

1. **Detection:** retain the last pre-change listing and first changed listing, their time intervals, and first observed sizes. Header-only creation is an observation, not automatically mask-on.
2. **Coarse:** at 5-second spacing, observe up to 90 seconds (≤18 listings) to bracket a subsequent coherent size step. Fit an initial physical cadence from observed intervals and the profile's broad prior; do not assert that this is flush number one.
3. **Ladder:** in a following predicted cycle, at most six 1-second-spaced listings across the coarse window. Estimate first/last visible file update edges. If the prediction window is too wide, remain coarse/degraded rather than extending the allowance.
4. **Fine:** in the next cycle, at most six 0.2-second-spaced listings around each of the two edges (≤12 total). Omit an edge refinement if HTTP service cannot finish before its next target. This explicitly replaces the old unconditional 1 request/s peak.
5. **Tracking:** transition to sustained adaptive refinement as defined above instead of a mandatory one-listing/coarse MAINTAIN schedule. Required liveness uses the same observations; do not add a second independent polling loop. Keep estimated uncertainty windows explicit and use spacing/backoff or reacquisition when actual service conditions require it.

An early gate already showing growth invalidates the prediction; widen/re-fit. A late gate missing growth may be jitter/cache/staleness: retain that uncertainty, never classify it alone as stopped therapy. Log actual issue/response times, not merely scheduled targets. Gate margins use the observed jitter and service/cache uncertainty, not a universal 0.3-second constant.

Safety takes priority over precision. If deliberate burst contact is unvalidated or unsafe, disable the ladder and use bounded outside-in quiet observations. Without a trustworthy prediction, even a supposedly quiet poll cannot be guaranteed not to contact a write. Do not claim zero contact. High jitter or uncertain caching disables fine timing; ordinary liveness continues at the bounded rate. Sustained best-effort refinement follows the explicit 500 ms-or-slower contract above, not a second independent polling loop; enabling it does not authorize uncharacterized 200 ms dense polling.

### 6.4 Accounting and expected latency

For P≈60 seconds, three planned requests per cycle correspond to approximately 180 listings/hour, 1440 over eight hours, or 1800 over ten hours, before housekeeping/recovery and cancellation of unneeded requests. Broader predictions can require additional spaced requests within the per-window limit. These are workload estimates, not allowances or measured utilization. There is no cumulative listing count at which therapy monitoring or refinement must stop.

Watch can cost up to 240 listings/hour and is reported separately. Measure actual request counts, service distributions, failures, misses, and long-duration behavior. Hardware testing MUST validate watch and sustained active load; healthy HTTP alone does not prove absence of SD contention. Reduce optional work when actual contention, response delays, or failures require it, not simply because a historic count is high.

Early timing target: a model-qualified estimate preferably within 5 minutes of actual start, and within 30 minutes when evidence permits. The approximately one-minute discovery delay plus acquisition means 5 minutes is a target, not a universal guarantee. If anchors, indexing, safe probe, or model validation are missing at 30 minutes, publish the concrete pending/degraded reason; never manufacture an offset to satisfy the target.

## 7. Therapy state and freshness

### 7.1 Events, state, and timeouts

Therapy state is `UNKNOWN | IN_THERAPY | OUT_OF_THERAPY`; `therapy_active` maps to null/true/false. Status is filesystem-activity inference, **not a clinical measurement of mask-on or mask-off**.

Recognize a potential session set as newly created EDF files in a monitored DATALOG folder. One new EDF is enough to fail closed into IN_THERAPY; do not wait for all BRP/PLD/SAD/EVE/CSL files or decode a hashed alias. A new folder without files is not enough. An initial boot scan is a baseline, not a creation event. Treat increase of a recognized sampled EDF as growth; unknown EDF growth also blocks transfers while classification is pending. Timestamp-only changes, shrinkage, or unrelated files trigger uncertain-write confirmation rather than a timing-comb sample.

Any new potential session file or coherent growth moves OUT/UNKNOWN to IN immediately. Use multiple files/record-sized deltas to strengthen timing confidence, **not** to delay blocking transfer. While IN, every valid growth refreshes its activity time and resets all no-growth confirmations.

Publish effective thresholds, using milliseconds:

```
expected_append_max_ms = max(62000, P_upper_ms + jitter_allowance_ms)
out_timeout_ms = max(125000, 2 * expected_append_max_ms + visibility_allowance_ms)
```

Until cadence/jitter/cache behavior is characterized, 62/125 seconds are provisional AS11-derived defaults, explicitly flagged. `P_upper_ms` is the upper model interval from §8; allowances are the validated profile limits, enlarged by observed delays. If no finite visibility bound can be justified, timed OUT confirmation is unavailable: publish UNKNOWN and block live downloads until evidence/profile recovers.

There is **no exit at 62 seconds**. To enter OUT, require:

1. At least `out_timeout_ms` without a relevant write; measure from the **latest possible** growth time, conservatively the successful changed-list response time when no tighter bound exists. Never use an optimistic fitted midpoint to start this safety timeout.
2. At least two complete, successful, non-growing observations of every relevant hot/active folder, separated by at least one expected append interval, plus successful parent discovery in the confirmation sequence.
3. A final all-hot/active and parent refresh at/after the timeout, with no new potential session files, uncertainty, parse error, or stale response detected.

When active growth is overdue, prioritize this confirmation sequence subject to request spacing, service guards, and backoff. New growth cancels it immediately. At startup UNKNOWN may become OUT after the same timed two-round no-write sequence from its initial complete baseline, even if no `last_growth` exists. Old static EDFs must not leave the device permanently UNKNOWN.

On an essential monitoring failure, SETUP entry, card reconnect, service/backoff monitoring gap, or unbounded stale listing: state becomes UNKNOWN, `therapy_stale:true`, and prior quiet confirmations are cleared. Recovery requires new growth or a fresh idle-confirmation sequence; an HTTP success alone does not restore OUT. Do not count repeated cached responses as independent fresh evidence if caching is known/indicated. Byte-identical successful listings are otherwise usable only under the characterized freshness assumption; undetectable card-side staleness remains a documented limit.

### 7.2 Freshness and transfer decision

- A successful listing atomically updates its folder scan time. HTTP/parse failures set `folder_stale:true` immediately. Cached responses expose age even when not stale.
- Required active-folder liveness is stale after `expected_append_max_ms + 30000` without a successful observation. For OUT/transfer eligibility, every relevant hot/active folder and parent discovery must be ≤90 seconds old and error-free. A cold source folder must also be refreshed to ≤90 seconds before GET.
- Hourly neighbor scans during IN do not make active-folder liveness stale; they do prevent claiming those neighbors were freshly excluded from an OUT decision until the confirmation refreshes run.
- Cached reads can return complete old data with explicit flags. `folder_stale` and `therapy_stale` are different scopes and MUST NOT be collapsed into a single field.
- `transfer_allowed` is true only when mode is RUNNING, EZShare is reachable, state is OUT, therapy is non-stale, relevant monitoring is fresh, and no post-check/live operation is pending. A source-specific pre-check can still reject a file even when this global field was true.

While a live GET occupies the sole HTTP worker, `transfer_allowed:false` prevents another live request. Therapy becomes UNKNOWN/stale if its observation age limit expires; a successful post-transfer confirmation can rebuild OUT. Do not preserve a false claim of fresh status merely to let a long download pass.

Expose `state_since` and `last_growth` as nullable UTC epoch ms, and `age_ms` from monotonic elapsed time even when UTC is unknown. On a boot with no growth, `last_growth`/`age_ms` are null; expose `quiet_since_age_ms` for startup idle confirmation. `confidence` is `unknown | provisional | validated`; `reason` is a stable machine-readable code with optional short explanatory text. File-set splits, pause/resume, and noon rollover do not imply an instantaneous clinical boundary.

For an intentional bulk pause only, retain the complete pre-transfer hot/parent observation round as a comparison baseline. An age-expired UNKNOWN status still blocks new work, but that round may be the first of the two OUT-confirmation rounds when the post-transfer round arrives, provided the required separation/timeout hold and there was no card discontinuity, failed listing, or detected write. This avoids an unnecessary extra two-minute wait after every long file while retaining pre/post source and activity checks. Any detected write resets confirmation as usual.

## 8. Time anchoring and drift estimator

### 8.1 Authenticated time exchange

Use `esp_timer` microseconds for every deadline, observation, and interval. Never use a step-able wall clock for therapy timeouts or transfer freshness. UTC is an attached mapping, not the scheduler's clock. Store integer timestamps; API uint64 values use decimal strings (§10).

After each authorized connection, and every 10 minutes while connected, SomnoTrace performs five two-way exchanges and commits the lowest-RTT usable sample. The wire contract is in §10.4:

- `t1`: SomnoTrace UTC microseconds immediately before writing the first TIME_REQUEST fragment.
- `m2`: SomnoBridge monotonic microseconds at receipt of the complete request.
- `m3`: SomnoBridge monotonic microseconds immediately before submitting TIME_REPLY.
- `t4`: SomnoTrace UTC microseconds at receipt of the complete reply.

For an exchange short enough to neglect oscillator-rate change:

```
rtt_us = (t4 - t1) - (m3 - m2)
anchor_mono_us = (m2 + m3) / 2
anchor_utc_us = (t1 + t4) / 2
anchor_uncertainty_us = source_uncertainty_us + rtt_us / 2 + timestamp_allowance_us
```

Require a non-negative RTT ≤250 ms and no SomnoTrace NTP step between t1 and t4. Source uncertainty ≤100 ms remains required for qualified anchors. A bounded source uncertainty up to 500 ms may be accepted for provisional/best-effort mapping and SOT, with its full uncertainty carried into intervals and a machine-readable `anchor_best_effort` flag. Reject source uncertainty >500 ms rather than silently shrinking it. Include timestamp/stack-submission delay measured on-device; initial allowance 5 ms is provisional, never a demonstrated guarantee. The ~10–30 ms BLE figure in the proposal is a target, not a constant to stamp into every anchor.

Reject stale exchange commits (>5 seconds), wrong correlation, impossible times, and samples from a connection that has lost authorization. A time-source discontinuity creates a new anchor interval; never interpolate across an NTP step or reboot. SomnoTrace explicitly reports its time-source epoch in TIME_REQUEST; this is time discipline, not catalog generation/versioning. Even with an unchanged reported epoch, an anchor inconsistent with the prior clock mapping beyond the combined uncertainty/oscillator bounds must create a time-discontinuity flag and must not be interpolated through as ordinary drift.

Map observations using an affine interpolation between bracketing accepted anchors in the **same uninterrupted monotonic domain**. Carry endpoint uncertainty and the characterized oscillator residual into the mapped interval. Outside brackets, extrapolate with an explicit oscillator bound, initially ±40 ppm unless device characterization supports a different conservative value. Do not invent an anchor at boot or assume calibration survived power loss. Calibrate oscillator scale from ≥3 mutually consistent anchors spanning ≥30 minutes; retain its uncertainty rather than just its point ppm.

No accepted anchor means UTC fields and drift are null; catalog/monotonic therapy FSM still work. Without anchors within two hours on each side of timing evidence, set `no_anchor_2h` and enlarge uncertainty. Estimate updates after a later anchor are allowed and persisted; a growth-stop record is observation-closed, not forever numerically immutable.

### 8.2 Headers and the one early probe

An early header probe is optional but necessary for a useful early clock-offset result. Permit **at most one body-started HTTP attempt per observed file set**, in a predicted quiet window with time for the entire bounded request before the next predicted burst. No periodic reads, second-file fallback after bytes start, or user-enabled bypass. If prediction/safety is not trustworthy, defer it.

- Parse EDF's fixed 256-byte header and signal headers, bounded at **16 KiB**. Validate header size `256*(1+n_signals)`, all numeric fields, dates, positive record duration, and samples-per-record. Maximum 63 signals. Do not trust a fixed per-model record byte count; compute `record_bytes = 2 * sum(samples_per_record)` for 16-bit EDF and verify observed growth against it. Reject unsupported EDF variants explicitly.
- With card/profile Range capability UNKNOWN or UNSUPPORTED, select the smallest candidate whose current listed full size is ≤16 KiB, and perform one full GET with an enforced 16 KiB read cap. If it grows beyond the cap, abort the probe and defer.
- With VERIFIED_206, make one `Range: bytes=0-16383` request (or a smaller known header bound). Require real 206, a consistent start/end/total Content-Range, and no more than the requested bytes. A short range at EOF is acceptable only when header fields are fully present and consistent.
- A 200 response to Range is a regression: abort before consuming its body, disable Range, and defer. Do not turn it into an uncapped full read.
- Range verification is a controlled idle characterization action, bound to current card connection/firmware and invalidated on reconnect or suspected replacement. It is not permission for general offset/resume GETs; version 1 has none.

After therapy, obtain missing headers while performing ordinary validated file downloads: parse the bounded prefix as it passes through RAM. `GET_DRIFT` itself is a local lookup and never starts a hidden GET. A session with no eligible early probe therefore has pending drift until its first useful post-therapy file is read; SomnoTrace reads drift metadata again after the session's last file.

### 8.3 Indexing, physical cadence, and interval fit

Keep **physical flush cadence**, **sample-time scale**, and **RTC drift** separate. A preliminary AS11 bus observation near 57 seconds is neither an AS10 calibration nor proof that 60 seconds of EDF samples elapsed in 57 wall-clock seconds.

For one verified sampled-file beacon, after header size H and record size R are known:

```
q = (listed_size - H) / R
T_q = T_1 + (q - 1) * P, for q >= 1
```

Use q only when size≥H and the payload is a whole number of records. An in-progress/torn size is not a complete-record timing point. Unknown/negative EDF header record counts are not a reliable growth counter. A jump by several records advances q accordingly; never replace it with one cycle or use GCD alone as proof of record size.

The mapping above requires validation that each completed record becomes visible on that beacon at its corresponding flush, with no unexplained initial batch, missing first record, rewriting, or timer/RTC-trigger mismatch. Retain the observed sizes until the header resolves them. If mapping is ambiguous, publish `flush_index_ambiguous` and do not calculate a supposedly precise session start. An existing file found after reboot can yield a retrospective interval only when this mapping and all intervening extrapolation uncertainty are defensible; otherwise `observed_late` keeps drift unavailable.

For a successful listing i issued at s_i and completed at r_i, whose snapshot may be cached by at most C, an observed q implies these conservative constraints:

```
T_q <= r_i                         when q >= 1
T_(q+1) > s_i - C
```

Expand these inequalities by the characterized per-edge jitter bound, and map all times through anchor intervals. A change between listings is bracketed by the previous request start minus cache bound and the next response end, **not** by two response midpoints. If cache age has no justified finite bound, this high-precision fit is invalid.

Implement a two-parameter feasible-region fit on `(T_1, P)` by intersecting these linear half-planes with the validated cadence prior. Use relative seconds near the session to avoid loss of numeric precision, and compute min/max projections for the published intervals. Empty intersection invalidates the rigid model; do not discard contradictory successful polls to force a fit. A separately validated jitter model may enlarge bounds or fit residuals, but must flag that change. Refit after each useful gate and at stop, not only at stop.

Use one beacon's phase for start estimation. Other files can help bound P, but their write-order delays are distinct: do not combine BRP/PLD/SAD first-update edges as if they had identical phase. Estimate separate per-file phase/delay, or keep them as consistency checks. Header-only appearance retains an unknown creation latency and does not magically supply an exact mask-on anchor.

### 8.4 Offset, sign, rates, and limits

For a verified record-completion-driven model:

```
T_start = T_1 - P - lambda_w
header_device_interval = [h, h + 1 second)
clock_drift = true_start - device_start
```

Here h is the raw AS10-local header interpreted using the confirmed AS10 timezone. `lambda_w` includes the selected beacon's write-to-list visibility delay. Use a profile's explicit latency prior, not an allegedly measured constant. The proposal's `[0.05,0.5] seconds` is only a **hypothesis**, and may exclude per-file write-order/cache delay; it cannot be a guaranteed bound before evidence supports it. Where justified only as a prior, mark output provisional and name the prior. Offset and latency are confounded without independent timing truth: pooling nights does not remove a common systematic bias, and a sum/difference of offset and latency is not automatically the clock correction.

If true start is bounded by [S_lo,S_hi], the model-conditional drift interval is `[S_lo-(h+1s), S_hi-h]`. Publish its midpoint as `clock_drift_ms`, its interval, random sigma only when supported by the fitted/statistical model, and the systematic latency/clock/timezone components separately. Do not call quadrature sigma a worst-case bound. When no justified start model exists, keep `clock_drift_ms:null`; a raw appearance/header pair with unknown startup latency is not a guaranteed ±1–2 second alternative.

- Publish `flush_period_ms` and its interval whenever the visible comb is defensible, even if offset is pending.
- Publish `timer_rate_ppm = (P / D_record - 1) * 1e6` only after the one-record-per-flush, sample-timer-driven relationship is validated. This field means **elapsed-time scale deviation**, not oscillator-frequency ppm: `true_elapsed = device_sample_elapsed * (1 + timer_rate_ppm/1e6)`. Otherwise it is null with `timer_model_unverified`. Never automatically interpret a 57/60 ratio as an ordinary small RTC drift.
- `rtc_rate_ppm` is null on SomnoBridge version 1. SomnoTrace may fit cross-night offsets with explicit timezone/manual-clock change-point handling. Do not pool across suspected clock steps or firmware/card/profile changes.
- Ambiguous/nonexistent DST local times, unconfirmed timezone, and inconsistent header/file grouping produce flags and null correction, not arbitrary hour shifts. Noon-day labels remain the raw AS10 folder names after any correction.

The proposal's approximately ±0.3-second start, ±0.6-second offset, and 2–5 ppm night-end rate are **conditional targets** for simulation/hardware evaluation. Preferred early acquisition, 30-minute degradation reporting, and post-session accumulation remain requirements; sub-second absolute truth cannot be promised from unverified software-only latency assumptions.

### 8.5 File/session association and metadata schema

Lookup drift by `{directory,name}`, not a timestamp guessed from an 8.3 alias. Each retained record contains exact source paths, the raw parsed header start, the observed segment boundaries, and applicable file tuples. Group files only after compatible parsed headers and observed activity establish association; keep uncertain groups separate. Across noon or toilet breaks, continue a growing file's evidence, and create a new group for a genuinely new header/file set. Do not merge solely because UTC estimates are close.

`GET_DRIFT` returns a record with these mandatory fields (nullable where stated):

| Fields | Meaning |
|---|---|
| `schema`, `record_key`, `files` | Schema 1; opaque evidence-journal lookup key; exact source paths and tuples to which it applies. Not a catalog/file version. |
| `estimate_state`, `observation_closed` | `pending | provisional | validated | invalid`; whether the observed growth segment has stopped. Closed does not mean the source can never append again. |
| `session_start_as10`, `timezone_rule` | Raw local EDF start and interpretation rule, or null. |
| `session_start_true_ms`, `start_interval_ms` | Point and `[lo,hi]` UTC interval, or null. |
| `clock_drift_ms`, `drift_interval_ms` | NTP minus interpreted device start, or null. |
| `sigma_start_ms`, `sigma_drift_ms`, `systematic_components` | Nullable statistical sigma; named prior/bound components and provenance, never fabricated zero. |
| `flush_period_ms`, `flush_period_interval_ms` | Fitted physical period, or null. |
| `timer_rate_ppm`, `timer_rate_interval_ppm`, `rtc_rate_ppm` | Qualified sample scale, or null; RTC rate null in version 1. |
| `record_duration_s`, `record_bytes`, `complete_records` | Parsed beacon layout and latest known complete-record count, or null. |
| `model_id`, `flags`, `evidence_available` | Characterization profile/assumptions, machine-readable failure/quality flags, raw journal availability. |

Required flags include `clock_drift_pending`, `timing_unvalidated`, `flush_index_ambiguous`, `observed_late`, `jitter_high`, `cache_unbounded`, `no_anchor_2h`, `timezone_unconfirmed`, `tz_step_suspect`, `low_coverage`, `a2_violation`, `timer_model_unverified`, `evidence_truncated`, and `metadata_missing_after_reboot` where applicable. Return current accumulated metadata during therapy and the latest observation-closed/refitted record afterwards. Never apply a newly computed record blindly to different header starts or a changed/replaced source file.

## 9. Bounded evidence and metadata persistence

Catalog rebuildability must not mean that a morning power loss silently destroys every night's timing result. Persist **timing evidence and estimates only**, separately from volatile catalog and transfer state. No TF card is required.

Allocate a dedicated 2 MiB encrypted NVS evidence partition and a separate 1 MiB encrypted NVS metadata partition. Use a bounded append journal of compact UTF-8 JSON lines, packed into ≤3072-byte payloads in 256 rotating blob slots. Each blob includes format version, monotonically increasing journal sequence, payload length, and CRC32 of payload; slot keys are fixed `b000`–`b255`. These journal ordinals locate evidence, not file/catalog versions, and are never comparison identity.

On boot, scan slots, reject invalid/torn blobs, order valid sequences, and start a new chunk with an explicit boot/monotonic-domain boundary. Every chunk refers to its domain's boundary record; no interpolation may cross it. If its required anchor/boundary chunks have rotated out, flag the affected evidence incomplete rather than use a previous boot's esp_timer scale. NVS handles atomic blob writes; no filesystem, rename protocol, or boot counter is needed.

Persist these records:

- `config`: firmware, characterization/model ID, non-secret timezone settings, card version, and active uncertainty priors.
- `poll`: monotonic issue/response times, directory, status, phase/purpose, observed sizes/raw stamps for changed monitored files, and enough unchanged beacon/quiet-round summaries to reproduce the interval/FSM decisions. Include initial monitored-file metadata once per observation segment, not full file contents.
- `header`: exact source name, raw start date/time, parsed layout, and validated record facts. Exclude patient/recording-ID text and samples.
- `anchor`: accepted exchange times, uncertainty, source epoch, and domain reference.
- `event`: creation, growth, idle confirmation, gaps, resume, backoff, service delays, configuration changes, and clock/model invalidation.
- `estimate`: the current §8.5 record and its source/evidence association.

Use an 8 KiB RAM batch; commit when full or every 60 seconds, and at observation stop, configuration transition, and before reporting successful post-download metadata. Flash work must not block time-sensitive HTTP targets. A queue/flash failure flags `evidence_truncated`/`persist_failed`; never silently claim full replay coverage. Power loss can lose the current ≤60-second RAM batch, and recovered records must expose that gap.

Retain at most 64 latest observation-group metadata records, each ≤8 KiB, in the metadata partition. Atomically replace that group's latest estimate after useful anchors/header parsing and at stop. Retain summaries after raw journal rotation; `evidence_available:false` distinguishes a surviving summary from replayable raw evidence. Expose oldest retained metadata/evidence boundaries and utilization; **no fixed number of days is guaranteed**. SomnoTrace must persist records when synchronizing, not rely on SomnoBridge as an archive.

Lookup uses exact paths and parsed header identity. If history is missing/evicted or a reboot left no usable anchors/indexing, return `METADATA_UNAVAILABLE` with a reason, not a zero drift or a carry-forward estimate from another file. Rebuilding a catalog does not fabricate old timing observations. Clearing/replacing configured card details invalidates old associations; an undetectable identical-looking swap remains a limitation of the available evidence.

No `.part` files, file-body hashes, downloaded markers, sync windows, or SomnoTrace acknowledgments are persisted as SomnoBridge transfer state. SHA-256 during streaming is transient. Estimate retention keys do not create a delta or version protocol.

## 10. BLE protocol version 1

### 10.1 Service and framing

The following private 128-bit UUIDs are assigned by this spec, not inherited from SomnoTrace:

| Item | UUID / properties |
|---|---|
| Service | `8ba7d100-6e4b-4bb4-9f1d-7306766e6f50` |
| Control | `8ba7d101-6e4b-4bb4-9f1d-7306766e6f50`; write-with-response and indicate |
| Data | `8ba7d102-6e4b-4bb4-9f1d-7306766e6f50`; notify |

All accesses require §3 authorization. SomnoTrace subscribes after encryption. Request ATT MTU 247, Data Length Extension, LE 2M PHY, and 15–30 ms connection interval; negotiated fallbacks including 1M/MTU 23 MUST remain correct. Never assume a particular speed. Protocol framing fits MTU 23, though low MTU will be slow.

Each characteristic value begins with this **16-byte little-endian header**, followed by its payload (≤ATT_MTU−3−16 bytes):

| Offset | Field |
|---:|---|
| 0 | `version:u8`, always 1 |
| 1 | `kind:u8` |
| 2 | `flags:u16`, START=1, END=2; all other bits zero |
| 4 | `request_id:u32`, nonzero and unique among outstanding requests on this connection |
| 8 | `offset:u64`, byte offset within logical JSON/file stream, or acknowledged next byte for ACK |

Kinds: `JSON_REQUEST=1`, `JSON_RESPONSE=2`, `FILE_BYTES=3`, `ACK=4`, `ABORT=5`, `TIME_REQUEST=6`, `TIME_REPLY=7`, `TIME_COMMIT=8`, `EVIDENCE_BYTES=9`. Control carries all kinds except FILE_BYTES/EVIDENCE_BYTES, which use Data. Time messages use START/END fragmentation with byte offsets like JSON. ACK/ABORT are fixed small single-frame messages.

JSON messages are UTF-8, fragmented START→END with contiguous offsets from zero. Assemble inbound JSON to ≤4 KiB within 30 seconds; reject overlaps, holes, unknown kinds/flags, reused outstanding IDs, and invalid/duplicate-key JSON. One inbound fragmented message at a time; ACK/ABORT may interrupt it. Response fragments use indications and wait for ATT confirmation. Complete a logical JSON response before indicating another; do not interleave its fragments with time replies. Metadata is streamed from the bounded response view, not assembled into an unbounded server JSON buffer.

Each request is `{"op":"GET_STATUS","args":{}}`; response is `{"op":"GET_STATUS","ok":true,"result":{...}}` or `{"op":"GET_STATUS","ok":false,"error":{"code":"...","retry_after_ms":0}}`. The binary header provides correlation. An asynchronous file/evidence request may produce a `STREAM_BEGIN` JSON response and later a `STREAM_END` response under the same ID; each is independently fragmented from offset zero. Its ID remains outstanding until STREAM_END/error or disconnect.

All uint64 sizes, offsets, record counts, monotonic/UTC microseconds, and UTC epoch milliseconds in JSON are **decimal strings**, not potentially rounded JSON doubles. Ordinary small counters/relative millisecond ages and finite ppm estimates may be JSON numbers. Null remains null. Parsers reject negative/overflowing unsigned values and non-finite estimates. Schema/protocol versions describe contracts, not file versions.

Support one live stream and one ordinary query at a time. ACK, ABORT, GET_STATUS, and time exchange remain serviceable during a stream without touching EZShare. Other live work returns BUSY. Local day/drift queries can be serialized between stream frames; copied views have bounded lifetimes. Rate-limit logical control requests to 5/s with burst 10, exempting valid ACK traffic from that limit. Time bursts have their own maximum of five exchanges per sync round and one round/minute.

### 10.2 Operations

| Operation / arguments | Result and rules |
|---|---|
| `GET_STATUS {}` | Firmware/protocol/security profile, mode, WiFi reachability/RSSI, paired state, complete §7 therapy fields, effective thresholds, anchor/estimator status, busy operation, load counters, memory/journal diagnostics. Local only. |
| `GET_CATALOG_STATUS {}` | State, folders_total/scanned, hot_ready, catalog_complete, oldest_scanned_day, base day/source, hot/active names, incomplete/evicted days, diagnostic_flags (including hot_window_mismatch), and last errors. Local only. |
| `LIST_RECENT_DAYS {days:N}` | N integer 1–36600. Return existing valid names in the inclusive calendar range `base−(N−1)` through base, plus all existing hot and separately active names, deduplicated/sorted newest-first; scan state per day, catalog_complete/hot_ready/date_uncertain. No card I/O. |
| `LIST_DAY {day}` | Complete copied entry set, day, folder_stale, scan_age_ms, nullable UTC scan time, and last error. Never a silently partial set. Unknown/nonresident days return NOT_FOUND/NOT_SCANNED respectively. No card I/O. |
| `LIST_DIRECTORY {directory}` | Complete live listing outside DATALOG only, including validated child-directory names and regular-file `{name,size,timestamp_raw}` metadata. Reject DATALOG or descendants; use LIST_DAY there. Enforce OUT/fresh monitoring; do not insert results into the catalog. |
| `GET_FILE {directory,name,expected_size?,expected_timestamp_raw?}` | One live full-file stream with server pre/post checks (§12). Canonical directory is explicitly supplied, including `A:\DATALOG\YYYYMMDD`; do not accept competing `day` and `directory` parameters. Optional expected timestamp distinguishes omitted comparison from an explicitly null value. |
| `GET_DRIFT {directory,name}` | Latest applicable §8.5 record or explicit unavailable reason. Local only; no probe, card read, or wait for a new estimate. |
| `GET_EVIDENCE {record_key}` | Authorized diagnostic stream of retained raw JSON-line journal records belonging to that group, with completeness/truncation fields. Never EDF bodies. Local storage only. |

`GET_STATUS` therapy fields: `therapy_active`, `therapy_stale`, `state_since`, `last_growth`, `age_ms`, `quiet_since_age_ms`, `expected_append_max_ms`, `out_timeout_ms`, `confidence`, `reason`, `transfer_allowed`. Status/drift reads may succeed before catalog READY. A local metadata error is not permission to access EZShare directly.

Errors are: `BAD_REQUEST`, `UNSUPPORTED_VERSION`, `UNSUPPORTED_OP`, `BUSY`, `NOT_FOUND`, `NOT_SCANNED`, `SCANNING`, `CATALOG_INCOMPLETE`, `STALE`, `THERAPY_ACTIVE`, `THERAPY_UNKNOWN`, `SETUP_ACTIVE`, `SIZE_MISMATCH`, `SOURCE_CHANGED`, `SOURCE_UNVERIFIED`, `PATH_INVALID`, `HTTP_ERROR`, `PARSE_ERROR`, `TIMEOUT`, `RESOURCE_LIMIT`, `BUDGET_EXHAUSTED`, `ABORTED`, `LINK_LOST`, `METADATA_UNAVAILABLE`, `STORAGE_ERROR`, and `SECURITY_NOT_PROVISIONED`. Security failures use ATT insufficient authentication/encryption and disconnect as appropriate, not an unauthenticated JSON data response. Errors include bounded redacted details and a retry delay where useful.

### 10.3 Streaming, flow control, and integrity

STREAM_BEGIN declares `{stream_type,directory,name,expected_size,cached_size_at_start,content_length,source_before}`. For evidence, substitute record_key/completeness and a known byte length if available. A missing HTTP Content-Length is null, not zero; the pre-listed expected file size still bounds the transfer. Start no Data notifications before this response is confirmed and the client grants credit.

Client ACK uses the stream request_id, `offset` as next contiguous received byte, and a little-endian `window_bytes:u32` payload. Initial ACK offset is zero. Window is 0–65536 bytes, initial recommendation 16384. Send only if `sent_offset + next_payload_length <= ack_offset + window_bytes`. Reject ACKs beyond sent bytes or decreasing offsets. Zero window pauses; lack of progress/credit for 10 seconds aborts. ACK at least every 16 frames or 250 ms; it acknowledges received/accepted bytes, not permanent local-file completion.

Data notifications carry contiguous FILE_BYTES offsets; START on the first and END on the last body frame, with a zero-length START|END frame for an empty file. Compute SHA-256 over exactly the transmitted file bytes. BLE link authentication/integrity plus offset/length checks make a second per-frame CRC unnecessary. A hole, duplication, wrong ID, or reconnect invalidates the stream: abort/restart at zero, not undocumented range resume.

For file streams, after EOF, wait for client acknowledgment of all body bytes, run mandatory post-checks, and send STREAM_END with `{ok,actual_bytes,sha256,source_before,source_after,source_unchanged,therapy_status,monitoring_checked,completion_allowed,drift_available,error}`. `ok` covers byte transport; **completion_allowed** covers source/activity validation. Both must be true before publication. Errors after STREAM_BEGIN are explicit STREAM_END failures when the BLE link survives. No EOF, END body frame, or matching hash by itself authorizes marking a file complete.

For evidence streams, STREAM_END reports actual_bytes, sha256, and evidence completeness/truncation only. It performs no EZShare post-check and grants no file-completion permission; local journal CRC/read errors fail the evidence stream explicitly. Evidence access must remain local even during therapy.

ABORT uses the target stream request_id and no payload; it releases buffers and marks the stream ABORTED. For file streams it also closes upstream HTTP and schedules monitoring recovery; aborting a local evidence stream causes no card request. It never deletes a local file on either device. ATT/link loss aborts likewise. No retransmission or file content is retained across reconnect.

### 10.4 Binary time messages

Each payload below is little-endian and can be fragmented via §10.1. Use one request_id for its exchange; cache only the last five pending exchanges for ≤5 seconds.

| Kind | Payload in order |
|---|---|
| TIME_REQUEST | `t1_utc_us:u64`, `source_uncertainty_us:u32`, `source_epoch:u32` |
| TIME_REPLY | echoed `t1_utc_us:u64`, `m2_us:u64`, `m3_us:u64`, echoed `source_epoch:u32` |
| TIME_COMMIT | echoed `t1_utc_us:u64`, `t4_utc_us:u64`, echoed `source_epoch:u32` |

SomnoBridge captures m2 on receipt of the complete TIME_REQUEST; SomnoTrace captures t4 on receipt of the complete TIME_REPLY and supplies it in TIME_COMMIT. Set m3 immediately before submitting the first reply fragment; subsequent fragmentation/confirmation delay is included in the measured RTT, not subtracted as server processing. Validate commits against cached exact t1/epoch/m2/m3, compute §8.1, then return JSON acknowledgment with accepted/rejected reason and anchor uncertainty. Do not accept caller-supplied m2/m3 or change monotonic time. A wrapped/changed source_epoch forces a new interpolation segment; it is not a persistent synchronization token.

## 11. Read-only EZShare HTTP adapter

### 11.1 Paths and endpoint construction

BLE/portal requests provide structured strings, never URLs. Canonical directories are volume-rooted `A:\` or `A:\component\...`, up to 240 ASCII bytes. Accept `A:` as a root spelling, normalize forward slashes to backslashes and the drive to uppercase, and remove one trailing separator except at root. Reject empty internal components, `.`, `..`, other drives, colon in components, NUL/control characters, `%`, and FAT-invalid characters. A filename is one nonempty component ≤255 bytes with no separator, traversal, encoded-input syntax, or trailing dot/space. Exact ASCII EZShare aliases are the version 1 supported download identifiers; an unsupported real entry is reported as a parse/unsupported-name error, never silently omitted from a purportedly complete listing.

Compare FAT directory names case-insensitively for policy checks. Recognize DATALOG regardless of input case. `LIST_DIRECTORY` cannot access `A:\DATALOG` or descendants; GET_FILE inside DATALOG requires exactly one valid day component. Preserve exact exposed file aliases for lookup/download; detect ambiguous duplicate aliases rather than pick one.

Construct only these GET endpoints on the fixed §4.3 card origin:

- `/dir?dir=<percent-encoded canonical A: directory>`.
- `/download?fname=<percent-encoded exact alias>&fdir=<encoded adapter directory>`.
- `/client?command=version`, once per association when permitted by scheduling/backoff.

The observed card adapter uses `fdir=..` for volume root and `fdir=..\<components>` otherwise, relative to its DCIM web root. That **one internal parent-prefix** is protocol translation, not a client-supplied traversal path. Confirm it for root, DATALOG, and SETTINGS with real-card fixtures before declaring the adapter supported. If a card variant uses a different verified spelling, select a fixed characterized adapter; never pass through arbitrary href/host/query strings.

Percent-encode each query value exactly once; parse/decode card links exactly once and validate their resulting origin, endpoint, fname, and directory against the containing listing. Never follow absolute external links, redirects, `/config`, `/photo`, write-like commands, or admin endpoints. HTML links are untrusted inputs, not executable instructions. Root navigation entries `.`/`..` may be ignored explicitly as navigation, not file rows.

### 11.2 Listing parser and limits

Use a bounded streaming HTML/preformatted-list parser, independent of SomnoTrace implementation. Parse complete `<pre>` entry rows, timestamps, decimal byte sizes or encoded `<DIR>`, and their validated links. Decode standard HTML entities once. Treat the timestamp field as raw text; if absent, use null. Do not substitute the HTTP Date header, zero timestamps, or guessed sub-second values.

A listing succeeds only on HTTP 200 with a complete, recognized listing structure and valid rows. A genuine empty `<pre>` can be an empty directory. A login page, truncated HTML, overflowed size, conflicting duplicate name, bad link, or unrecognized file row is PARSE_ERROR. Never interpret a failed/partial listing as an empty directory. Retain fixtures for actual card firmware variants; expected syntax comes from evidence, not this illustrative table alone.

Default limits: listing body ≤192 KiB, one row ≤2048 bytes, raw stamp ≤96 bytes, header bytes ≤8 KiB, and bounded total entries within scratch/catalog limits. Over-limit input returns RESOURCE_LIMIT and marks the folder stale. Requests have 2-second connect timeout and 5-second total listing deadline. Record actual monotonic request/response times even on failure. Do not retry within the same scheduled slot or extend a dense window to catch up.

Send `Cache-Control: no-cache` and `Pragma: no-cache`; accept them as requests, not evidence the card obeyed. Characterize cache behavior in §14. Reuse a keep-alive connection only when the card supports it correctly; after timeout, malformed response, mode transition, or abort, close it and resynchronize. No HTTP request is simultaneously active on a second connection.

### 11.3 File bodies

Use IDF HTTP parsing for valid Content-Length or chunked/close-delimited bodies. Reject ambiguous/conflicting framing headers. Expected MIME is binary, but do not rely on MIME alone; reject an identified HTML/login/error body and non-success status. EOF must be consistent with HTTP framing and the pre-listed size. Version 1 ordinary GET accepts HTTP 200 only; 206 is solely the verified early-probe path.

For known Content-Length mismatching pre-list size or supplied expected_size, fail before sending body notifications. Without Content-Length, stream only up to the pre-listed expected length, inspect completion/one-extra-byte as necessary, and fail if shorter, longer, truncated, or framing-invalid. Limit headers to 8 KiB, no-progress body time to 10 seconds, and total live GET to 30 minutes. Backpressure must not grow memory without bound; if the card times out while BLE is paused, report failure and restart later, not spill to TF.

Do not claim a matching length/hash detects a same-size rewrite or makes a growing source immutable. SHA-256 verifies the transmitted bytes, not that they form a final AS10 session. SomnoTrace validates downloaded EDF format/record integrity separately.

## 12. File synchronization and resume races

### 12.1 Server transaction

SomnoBridge enforces this sequence for **each** ordinary GET_FILE:

1. Validate authorization, mode, paths, current OUT status, and global monitoring freshness when the request reaches the worker; an earlier client status read does not reserve permission.
2. Capture a complete fresh pre-transfer round of all relevant hot/active folders and parent; reuse successful observations ≤90 seconds old if they still satisfy the OUT proof. Preserve this baseline for §7's intentional-pause rule.
3. Obtain a fresh complete source-directory listing (or reuse that same round when applicable). For DATALOG, atomically refresh the catalog; outside DATALOG retain the source tuple only for this operation. Resolve the exact name and pre-size/stamp. If any scan discovers writes or yields unknown/stale evidence, reject before body.
4. Compare supplied expected_size and, if supplied, expected_timestamp_raw against the **live pre-tuple**, not just the old catalog. A mismatch is SIZE_MISMATCH/SOURCE_CHANGED. If expected_size is omitted, the fresh pre-size becomes the mandatory announced expected length; the client must compare STREAM_BEGIN to its intended source tuple before granting credits.
5. Start one full HTTP GET. Check framing/length before STREAM_BEGIN; `cached_size_at_start` is the catalog value at request admission for DATALOG, null otherwise. Announce source_before and actual Content-Length (or null); stream through the 64 KiB RAM ring while honoring credits.
6. At EOF/acknowledged body completion, prioritize an immediate **source-directory refresh**, then every relevant hot/active folder and parent not covered by that source refresh. These are internal validation requests and may execute even when public live APIs are currently blocked by UNKNOWN/IN; do not route them through LIST_DIRECTORY's public OUT gate.
7. Require matching pre/post source tuple, exact byte length, successful monitoring, and a rebuilt/continuing non-stale OUT decision. New file/growth/uncertain writes, a missing source, error, unresolved confirmation, or a stale monitoring decision makes completion_allowed false. No next GET starts until this decision is sent and buffers released.
8. Persist useful parsed header/estimate updates and return STREAM_END. Post-validation remains required for large historical files and root files, not only current DATALOG.

A body may finish even if status ages into UNKNOWN. Resume cannot normally be discovered while that sole HTTP request is active; detection occurs at its post-check. Do not state that SomnoBridge immediately notices mask-on. On a client abort, WiFi loss, or deadline, close the request, recover monitoring, and return failure if the BLE link survives. Partial data is not a successful file.

Completion_allowed may be false despite byte transport succeeding. SomnoTrace may keep the `.part` unverified or discard it; it cannot rename/mark complete based on transport alone. No server polling limit is relaxed to salvage a download.

### 12.2 SomnoTrace integration contract

SomnoTrace implements the central-side UI/protocol independently; this spec does not assert its current firmware already supports pairing or these UUIDs.

1. Pair by entering the LCD passkey, persist its bond, and reconnect with encryption.
2. Supply periodic usable time anchors; read GET_STATUS and GET_CATALOG_STATUS.
3. Request LIST_RECENT_DAYS with configurable N (initial 14) and LIST_DAY for the returned scanned days. A failed/unscanned/partial result is not evidence of no files.
4. Compare `(day,name,size,timestamp_raw)` against its durable downloaded tuple. Download changed/missing files only when allowed, smallest first and BRP last.
5. Write to `.part`, validate offsets, expected/actual lengths, SHA-256, and EDF integrity. Atomically publish and record the **source tuple**, not merely `downloaded[path]=true`, only after completion_allowed and local validation succeed.
6. Query GET_DRIFT after the relevant final file download and persist the latest applicable metadata, including pending/degraded states. If an offset remains pending, preserve raw files and permit a later metadata query; file completeness and timing quality are separate.
7. If relevant DATALOG files changed, refresh selected root/STR/SETTINGS resources with live LIST_DIRECTORY/GET_FILE. Re-fetch STR conservatively after such changes even when size/stamp is unchanged until rewrite semantics are validated.
8. Recheck recent N days on every sync. A later remote size or raw-stamp change makes an already completed path eligible again. Support a manual wider window/full rescan for older files.

Do not automatically delete local files after remote disappearance, renamed aliases, changed card, INCOMPLETE catalog, or a smaller query window. Same-size/unchanged-stamp rewrites are fundamentally invisible to comparison; conservative re-fetch of known rewrite-prone types mitigates them but is not a universal content-change detector.

### 12.3 Applying drift downstream

SomnoBridge never rewrites source or downloaded EDF bytes. SomnoTrace preserves raw provenance and applies metadata only to its generated/exported timeline under its own verified EDF rules:

- Positive clock_drift_ms means add it to interpreted AS10 event timestamps to reach true time.
- Preserve raw noon-day folder labels; corrected time crossing noon does not relabel the reporting bucket.
- Keep timer sample-scale correction distinct from RTC-stamped calendar/event correction. A pending/null timer rate must not be treated as zero measured rate.
- Plain EDF's second-resolution start and minute-resolution STR fields impose downstream quantization; preserve millisecond true start and uncertainties separately before rounding.
- Export consistency across header, filenames, STR event fields, annotations, and any CRCs belongs to SomnoTrace acceptance tests. A radio bridge spec alone does not authorize arbitrary EDF/STR rewriting.

No inactivity rule can exclude a toilet break followed by resume after successful post-validation. With append-visible or stamp-visible changes, the next catalog comparison catches the newer tuple and causes a replacement download. This is the intended simple recovery, not a guarantee of immutable snapshots or perfect clinical stop detection.

## 13. Runtime layout, diagnostics, and recovery

### 13.1 Minimal component boundaries

Use a thin `main` and first-party modules for board/UI, config/security, EZShare adapter/scheduler, catalog/activity, time/estimator/journal, and BLE protocol. Names and internal C interfaces may follow normal ESP-IDF conventions; the public wire/data contracts above are fixed. Pure parser, FSM, date-selection, fit, and framing logic MUST be host-testable without radios.

Use these execution owners rather than a task per file/subsystem:

| Owner | Work and boundary |
|---|---|
| IDF WiFi/NimBLE tasks | Normal stack ownership. GAP/GATT callbacks validate/enqueue/capture timestamps, never block on card, NVS, or display. |
| Card worker | Sole HTTP owner; priority queue, scheduled deadlines, streaming producer, and pre/post validation. Publish observations to state processing. |
| State/time worker | Atomic catalog replacement, therapy FSM, estimator, time mapping, and bounded API response snapshots. Long fits must yield/cooperate. |
| BLE stream sender | Drain ring under credit limits; compute transmitted-byte hash and track offsets. |
| UI/journal service | Button/display and queued flash commits; may be separate small tasks when necessary to avoid flash/display blocking precise poll targets. |

Scheduling priority must favor radio stacks and prompt timestamp capture over display/flash work. Begin without application core pinning; introduce it only after measured coexistence/timing evidence justifies it. Absolute esp_timer targets are planned times, not proof of actual issue time. Never run a blocking HTTP/NVS operation in a timer interrupt.

Initial PSRAM allocations: catalog ≤768 KiB, listing scratch ≤192 KiB, one response view ≤128 KiB, file ring 64 KiB, evidence/queues ≤32 KiB. These total ≤1184 KiB, leaving headroom in 2 MiB for allocator/other users. Request JSON is ≤4 KiB, HTTP headers ≤8 KiB, and LCD uses a small DMA-capable internal-RAM stripe rather than an extra full-screen framework. Crypto/radio/DMA buffers and task stacks MUST use the appropriate internal-memory capabilities.

Bring-up must measure total/largest free internal and external heap, stack high-water marks, fragmentation, and worst-case combinations, not assume the above allowances guarantee fit. All queues are bounded (card work ≤16 items, observations ≤16, journal batches ≤4); reject excess client work with BUSY. Coalesce refreshes; never drop an essential observation silently. Observation overflow marks a coverage gap/UNKNOWN and disables fine timing until recovered.

Require a 10-second task watchdog and progress reporting from application tasks, enabled brownout detection, and bounded waits/HTTP deadlines. Idle/blocked tasks must yield normally. A long but progressing credited transfer is not a deadlock; monitor its progress rather than disabling the watchdog globally. On boot show the last reset reason. Neither watchdog reset nor NVS init error permits automatic factory erase.

### 13.2 Display and diagnostics

The LCD must show, in readable pages:

- Normal: EZShare connection/reachability, RSSI, authorized BLE link, therapy true/false/unknown and observation age.
- Timing: drift or pending reason, quality/uncertainty, anchor age, and model flags; never show null as `0 ms`.
- Transfer: short filename/alias, bytes/expected length, measured throughput, validating/deferred/error state.
- Diagnostics: firmware/security profile, catalog state/usage, load/backoff status, last error and reset reason.
- Enrollment/setup: pairing PIN or AP SSID/password/address and a visible timeout. Do not overlap the two enrollment channels or show stale secrets after closure.

Normal redraw rate ≤2 Hz; transfer progress can update at that rate. Wake the LCD for pairing/setup and faults; turn off enrollment secrets immediately on timeout/success. Keep logs at INFO by default, with bounded redacted error context and optional DEBUG in developer builds. No raw HTTP/EDF or credentials in console/crash dumps. Sensitive memory dumps require a separate opt-in diagnostic policy, not default production behavior.

Expose diagnostics to the bonded peer through GET_STATUS; AP status is redacted as §4 specifies. No cloud logging, telemetry, arbitrary shell, OTA endpoint, filesystem browser, or unauthenticated evidence download. Failure reason codes must identify card reachability, parser, stale data, model invalidity, memory, time anchor, and security provisioning separately.

### 13.3 Recovery behavior

| Event | Required recovery |
|---|---|
| BLE disconnect | Abort transient stream, keep monitoring, retain journal; client restarts file from zero and supplies a new anchor after reconnect. |
| WiFi/card loss | UNKNOWN/stale, stop live work, backoff; rediscover parent/hot state before OUT can recover. |
| Reboot | Validate settings/security, recover journal/summaries, rebuild volatile catalog; no inherited OUT state or file-complete claim. |
| Bad settings | Visible FAULT/setup-required state; physical hold opens protected setup. Never print/erase secrets automatically. |
| PSRAM allocation failure | Explicit INCOMPLETE/RESOURCE_LIMIT; retain usable complete data, suspend precision as needed. |
| Journal failure/full rotation | Explicit persistence/truncation diagnostics; metadata still carries quality flags. No pretending raw history exists. |
| Resume detected | IN, clear quiet proof, reject new live work; failed post-check prevents downloaded-file publication. |
| Suspected clock/card step | Start a new model/association segment; retain provenance, do not pool old and new correction. |

Version 1 flashing/recovery is local USB ROM/IDF tooling. Do not expose unsigned network updates as a shortcut. Production signing/eFuse operations are out-of-band deployment actions requiring explicit operator approval, with compatible recovery tested before use.

## 14. Verification and release gates

Verification must distinguish software correctness, measured hardware behavior, and assumptions that cannot be calibrated without wires. No real patient data in fixtures, reports, screenshots, or committed tests. Synthetic headers/listings are the default; sanitize real-card observations locally before sharing.

### 14.1 Host acceptance tests

| Suite | Required cases/assertions |
|---|---|
| Parser/path | Root/DATALOG/SETTINGS requests; alias links; entities; missing stamp; empty listing; malformed/truncated HTML; conflicting duplicates; uint64 overflow; maximum row/body; login page; external/redirect/admin links; raw/encoded traversal and drive confusion. Failure retains old cache and marks stale. |
| Catalog/date | Gregorian validity, noon boundaries, DST calendar arithmetic, ±1 hot days, unknown time fallback, multiple active folders, new parent discovery, startup partial/READY/INCOMPLETE/ERROR, atomic replace, cold eviction, oversized day, no generation/per-entry version fields. |
| Scheduler | All poll purposes/retries counted diagnostically; no HTTP concurrency; 200 ms peak only inside authorized fine windows; guarded 500 ms-or-slower sustained refinement; 1-second ordinary spacing; legacy dense-acquisition/per-minute peak and per-window limits preserved; no cumulative listing cap or NVS load-budget checkpoint gate; actual service/backoff staleness cannot produce OUT. |
| Therapy | Startup stable files reach OUT only after full proof; new EDF blocks immediately; no 62-second exit; 125-second/default and adapted thresholds; catch-up growth; timestamp-only/shrink uncertainty; missing/stale/cached listings; reconnect; multiple folders; pause/resume during GET; age expiry; pre/post intentional-pause recovery. |
| Protocol | MTU 23/247 fragmentation; order/duplicate-key/oversize rejection; security permissions including CCCDs; IDs; uint64 decimal strings; flow-window bounds; notification hole/duplicate; zero file; abort, credit starvation, error after STREAM_BEGIN, no implicit resume. |
| Persistence | Torn writes, settings commit failure, key unavailable, bond/identity inconsistency, journal rotation, metadata eviction, boot-domain separation, lost RAM batch, explicit missing metadata, no persisted download ledger. |
| Download | Content-Length match/mismatch/absent; valid/invalid chunked framing; premature EOF/extra bytes; source removed/shrunk/grown/rewritten; source folder different from hot folder; root files; post-check failed/stale/active/unknown; `.part` remains uncommitted unless every gate passes. |

### 14.2 Estimator simulation gate

Before enabling a timing profile, exercise the **actual planned scheduler and estimator**, not an idealized periodic sampler. Generate known true starts, header floor quantization, physical cadence, file-order delays, both rates, bounded/unbounded latency, cache age, service/poll jitter, anchor asymmetry/steps/gaps, missed/coalesced/torn records, late discovery, DST ambiguity, and mid-session steps.

Required assertions: correct flush indexing (including discovery after several records), declared interval coverage under the declared model, correct NTP-minus-device sign, no rate inference without record/flush validation, no interpolation across boot/clock epochs, correct uncertainty growth on extrapolation, empty fit→invalid, and no unqualified precision when a systematic latency assumption dominates. Report bias, random spread, model-conditional interval coverage, time-to-first-result, request counts, and fallback rates separately. Check that repeated sessions do not incorrectly average away a shared latency/cache bias.

The lead responsible for timing correctness must approve simulator equations, distributions, acceptance thresholds, and reported conclusions; do not derive an apparently reassuring metric from an unreviewed sampling model. Preserve simulation inputs/results so later presentation does not silently recompute different evidence.

### 14.3 Hardware characterization gates

| Gate | Measurement / release condition |
|---|---|
| H0 — board/security/setup | Verify GPIO/display/button, runtime versus boot-strapping hold, protected AP lifecycle, settings after power cycle, level-4 passkey pairing/reconnect, unauthorized/legacy/second-peer rejection, physical forget, NVS encryption provisioning failure paths. |
| H1 — EZShare baseline | Real listing syntax/aliases/root fdir, sizes/stamps/cache behavior, normal watch load, service distributions, card-version variation, complete-record visibility, body framing, and checksum/SD-error evidence. Low request rate still needs validation. |
| H2 — timing relationship | Physical period versus record accumulation; initial file/header/first-flush relationship; timer-driven versus RTC-minute flushes across ≥10 sessions; header start versus STR MaskOn (minute resolution); batch/torn/catch-up behavior. No sub-second offset profile before the selected model is supported. |
| H3 — safe acquisition | Deliberate burst-contact tests under controlled conditions, then inspect EDF record counts/CRCs and AS10 SD errors. Dense acquisition ships only if safety and service-time limits pass. Otherwise disable it and report degraded timing. |
| H4 — early header/Range | Verify 206 + exact Content-Range and 200 fallback rejection, cap enforcement on growth, full ≤16 KiB probe safety, quiet-window duration, card/reconnect invalidation. Range is never assumed from a header alone. |
| H5 — actual RF coexistence | On GEEK with BLE+WiFi active, measure actual poll placements/service time, anchor error, throughput/backpressure, watchdog/heap, and fit coverage. Validate with negotiated low MTU/1M as well as preferred settings. |
| H6 — lifecycle | Full-night load ≤cap, stopped/resumed therapy during largest GET, next-day/noon crossing, powered-off/brownout reboot, old-card crawl, oversized catalog, bad WiFi credentials, missing anchors, and identical-looking swap limitations. |
| H7 — consumer/export | Independent SomnoTrace central integration; file comparison/replacement and drift retrieval; downstream EDF/STR/sign/CRC/rounding behavior accepted by intended consumers without moving raw noon-day labels. |

H0/H1/H6 are gates for a useful secure catalog/file bridge. H2/H3/H4/H5 plus simulation gate the corresponding early/high-precision timing capabilities. A useful degraded bridge may ship without Range or dense timing, but must label pending/unavailable drift honestly. E-sim/E0/E0b/E-FSM/E1/E1c/E2/E3/E8/E9 in proposal-final remain research context; H0–H7 are their implementation-facing consolidation, not claims they have already passed.

### 14.4 Implementation sequence and definition of done

1. Independent IDF board skeleton, readable LCD/button, protected NVS, setup AP, and secure BLE pairing.
2. Host-tested read-only HTTP adapter/path parser and shared scheduler.
3. Volatile catalog, hot discovery, startup/idle therapy proof, status and cached metadata APIs.
4. Bounded BLE streaming, server pre/post validation, and independent SomnoTrace client integration.
5. Authenticated time anchors, bounded journal, header parsing, and pending/degraded drift API.
6. Simulator and characterization; enable only validated cadence/index/latency profiles and safe staged acquisition.
7. End-to-end night/reboot/resume tests, measured resource/load budgets, and downstream export verification.

Firmware is complete against this contract when these behaviors pass their applicable gates, limitations are exposed on LCD/AP/BLE, and SomnoTrace can recoverably download changing recent files plus applicable drift metadata without snapshots, mandatory TF, hidden generation state, or unauthorized access. Passing a build alone is not completion. A pending model is acceptable degraded output, not evidence the early drift goal has been achieved.

## 15. Hardware/platform references

These are behavioral/hardware references, not permission to copy firmware code:

- [Waveshare GEEK documentation](https://docs.waveshare.com/ESP32-S3-GEEK), [product page](https://www.waveshare.com/esp32-s3-geek.htm), and [resources/schematic index](https://docs.waveshare.com/ESP32-S3-GEEK/Resources-And-Documents).
- [Waveshare schematic](https://files.waveshare.com/wiki/ESP32-S3-GEEK/ESP32-S3-GEEK-Schematic1.pdf) and [ESPP GEEK documented pin map](https://esp-cpp.github.io/espp/dev_boards/waveshare/ws_s3_geek_example.html), used for pin/hardware facts only.
- [ESP-IDF v5.5.1 NimBLE guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/bluetooth/nimble/index.html) and [NVS encryption guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/storage/nvs_encryption.html).
- SomnoTrace `spec/archive/files-over-http.md` (EZShare endpoint behavior) and `spec/0002-edf-export.md` (consumer sign/raw noon-day conventions), consulted as documentation only. SomnoTrace's HTTP-server design, open endpoints, board pinout, and code are not adopted here.
- CPAP-AutoSync SD-bus observation cited by proposal-final: preliminary AS11-only cadence evidence, not an AS10/EZShare constant. hms-cpap is not used as evidence.
