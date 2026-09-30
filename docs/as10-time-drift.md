# AS10 Time Drift — Problem Statement & Investigation Map

> **Status:** problem definition. This document states what is broken, what we
> can observe, and *where* to look for a measurement strategy. It deliberately
> avoids committing to a solution — the right approach should fall out of the
> side-channel inventory below.

---

## 1. The problem

SomnoTrace talks to the ResMed **AirSense 11 over BLE**. The **AirSense 10 has
no radio** — its only data path is the SD card. To support it, the AS10's card
slot hosts an **EZShare WiFi SD card**, and **SomnoPal** lives permanently on
that card's access point, proxying files to SomnoTrace over BLE.

The problem: **the AS10's internal clock drifts** (reports put the error up to
~20 minutes), and it stamps that clock into everything it writes — FAT
directory entries, EDF file headers, possibly session data. Before SomnoTrace
can trust the timeline in an AS10 recording, we need to know *how far* the
AS10's idea of "now" deviates from real time. Only then can we correct the
timestamps on ingest.

The constraints that make this interesting:

- **EZShare is read-only for us.** We get its HTTP interface (`/dir` listings,
  `/download`) — we cannot write files, set its clock, or touch its config.
- **EZShare likely has no meaningful clock of its own.** Anything the card
  stamps itself is probably wrong or epoch-relative — unverified assumption,
  but plan for "no usable EZShare time."
- **SomnoPal does have real time.** NTP on the EZShare network... no — the
  EZShare AP has no upstream route. SomnoPal's NTP source is its *own* uplink
  side, not the card's network. This has an implication worth stating plainly:
  **SomnoPal may need two networks** (the EZShare AP for card access + some
  path to NTP), or a stored RTC discipline that survives without NTP. Open
  question — see §6.
- **Passive observation only.** Everything we can learn comes from *watching*
  the card's filesystem over HTTP and correlating with SomnoPal's clock.

---

## 2. Clocks in play

| Clock | Source | Trust | What it stamps |
|---|---|---|---|
| **AS10 RTC** | Internal, drifts ~min/month(?), user-settable, can step | ❌ the thing we're measuring | FAT mtimes, EDF `startdate`/`starttime`, EDF+ annotation onsets, possibly filenames/dir names |
| **EZShare card** | Unknown — likely none/free-running | ❌ assume useless | HTTP `Date:` header (if any), any card-generated metadata |
| **SomnoPal** | NTP (when reachable), free-running between syncs | ✅ ground truth | poll timestamps — "when did we observe X" |
| **SomnoTrace** | Has its own RTC story (AS11 path already solves this) | n/a | ingest-time corrections — downstream consumer |

The core measurement problem is therefore:

> **Pair "what the AS10 claims the time is" (observed via file content/metadata)
> with "when SomnoPal actually observed it" (NTP time).** Every usable side
> channel produces one such pair; the drift estimate comes from accumulating
> and reconciling them.

Resolution note: FAT32 mtimes have ~2 s granularity (mtime is in 2-second
units). Coarse, but workable — see §5.

---

## 3. What we know / invariants

Facts and working assumptions to lean on:

- **Periodic appends exist.** Some AS10 files are appended to on a ~60-second
  cadence during a therapy session. Files updating at a known rate are *timing
  beacons*: each append is an event we can observe and then read AS10-claimed
  timestamps from.
- **EDF internals are parseable.** EDF headers carry `startdate`/`starttime`;
  EDF+ files may carry annotation onsets. SomnoTrace already has an EDF stack
  (`as11_time.c` etc.) — parsing AS10 EDFs is not new science.
- **The AS10 writes the FAT.** SD access is block-level — directory entries
  (and their mtime fields) are written by the *host* (the AS10), not by the
  card. If so, **the mtimes in `/dir` listings are the AS10's clock, in the
  open** — potentially the single most direct channel available. *This is an
  assumption to verify, not a fact* (see experiment E1).
- **Blind intervals exist.** The card may power down with the AS10, and no
  writes happen outside therapy → gaps in the observation stream.

---

## 4. Channel taxonomy

Two classes of signal:

1. **Content channels** — AS10-time *claims* embedded in the data (EDF headers,
   annotations, filenames). These tell us what the AS10 thinks "now" is.
2. **Event channels** — *real-time* occurrences we can timestamp on SomnoPal
   (a file appeared, a file grew, the card came up). These anchor the content
   claims to wall time.

A drift measurement needs at least one of each, correlated.

---

## 5. Side channels to explore

Each entry: what it is → what it could yield → what to probe.

### C1 — FAT mtimes in `/dir` listings

The `/dir` HTML listing shows per-file timestamps. **Whose clock wrote them?**

- If the AS10 wrote the directory entries (expected — block-level SD), mtime =
  AS10's clock at last-write. Then `drift ≈ mtime − observation_time` the
  instant we observe a change — a direct, cheap drift sample every append.
- If EZShare rewrites/intercepts metadata (would be unusual but must be ruled
  out), mtimes reflect the *card's* clock — a different (probably useless, but
  still informative) channel.

**Probe:** compare a file's FAT mtime against the EDF `starttime` inside the
same file. Consistent → both are AS10's clock → C1 is live. Wildly
inconsistent → suspect EZShare interference.

### C2 — Write-observation events

Every poll diff (`size` grew, `mtime` changed, new file appeared) is a
**real-time event**: the AS10 wrote *between the last two polls*. Whatever
AS10-time claim we extract from that write gets anchored to an NTP-observed
window of width = poll interval.

**This is the fundamental anchor mechanism.** Poll interval bounds event
resolution — and periodicity can beat the interval (C3).

**Probe:** poll `/dir` during a live session; log `{ntp_time, filename, size,
mtime}` at ~5–10 s cadence; build the event series.

### C3 — Append-cadence phase analysis

If a file appends every ~60 s, polling at *any* reasonable interval eventually
sees every append — and because the appends are periodic, their true instants
are constrained to a comb in NTP time. Estimating that comb's phase from N
observed appends resolves *each* append instant far better than the raw poll
interval (sub-second is conceivable). Each well-localized append then upgrades
the precision of whatever AS10-time claim we pair with it.

**Probe:** from C2 logs, check whether append deltas lock to ~60 s; estimate
phase stability (does the comb drift? jitter?). Beware: FAT 2 s granularity
quantizes C1-derived timestamps, adding structured noise to pair with.

### C4 — EDF header `startdate` / `starttime`

When a new session EDF appears, its header claims when recording started. The
file's *appearance* is a real-time event (C2). `header_start − appearance_ntp`
bounds the offset — underdetermined by startup latency (unknown lag between
"AS10 stamped this" and "file visible on card"), but a strong upper bound and,
repeated over sessions, a consistent signal.

**Probe:** enumerate which EDFs appear at session start vs mid-session vs end;
for each, diff header start vs first-observed NTP time. Look for tight
clustering (low write latency) vs spread.

### C5 — EDF+ annotation onsets (content-level events)

EDF+ TAL annotations carry onset timestamps in AS10 time, describing events
that happened *during* the session (apneas, leaks, mask events). If an
annotation's *on-disk appearance* can be time-bound (the file grew at poll T;
the new bytes decode to an annotation claiming time τ), then `τ` vs the
real-time window when the event *actually occurred* is another pair — and
potentially a rich series, since events arrive throughout the night.

**Probe:** tail a growing EDF+; diff the new bytes each poll; decode and
compare claimed onsets against observation windows. Caveat: buffering — the
AS10 may batch-write records, so an annotation's claimed time may precede its
on-disk appearance by a variable lag. Characterize the lag's distribution; a
tight minimum is still useful.

### C6 — File/dir naming and creation ordering

ResMed cards organize data in date-/session-derived structures (e.g.,
`DATALOG` hierarchies). If names encode AS10 dates, they're free content
claims. And even where names carry no time, **creation order + sizes give a
relative timeline**: with a single absolute anchor, a sequence is
reconstructible.

**Probe:** dump a real card's full tree listing; catalog which names embed
dates and which don't.

### C7 — Periodic files inventory (the beacon set)

Not all files tick. Enumerate the set that appends/rotates on a schedule —
candidates include session tracking files (`STR.edf`-class), per-day DATALOG
files, summary/index files. The beacon set determines how dense our C2 event
stream is. Multiple files ticking at different phases = more real-time anchors
per hour.

**Probe:** same live-session polling as C2, across the whole tree; classify
each file: never-changes / appends-periodically / atomic-rewrite / appears-once.

### C8 — EZShare's own clock (probably dead — verify cheaply)

- Does the HTTP server emit a `Date:` header? What does it say?
- Does the card write any self-generated metadata visible in listings
  (hidden dirs, config files with card-stamped times)?
- If the card free-runs from boot, its time may still be *monotonic* — an
  uptime reference, not wall time. Monotonic anchors still bound rates.

**Probe:** capture raw response headers; compare listing timestamps for
card-managed vs AS10-written files. One cheap experiment, then likely close it.

### C9 — Card/AS10 power boundaries as ground truth

The EZShare lives in the AS10's SD slot — its power (and WiFi) dies with the
machine. **Card-up/card-down transitions are real-time events** observable by
SomnoPal, and they bound session start/end. Session boundaries also correlate
with EDF header/footer content → another pairing opportunity, this one
session-granularity rather than per-append.

**Probe:** correlate WiFi association state with first/last write events; check
whether the card stays up between sessions (EZShare cards have always-on AP
modes — need hardware facts).

### C10 — Secondary/exotic

Cheap to check, low expected value — keep on a list so we don't forget:

- HTTP listing staleness/caching behavior (noise model for C2, not a channel)
- `/client?command=version` — card firmware fingerprinting (matters for
  deciding whether C1 is trustworthy per-firmware-version)
- Connection timing anomalies (unlikely)
- Multiple EZShare firmware variants behave differently → may need
  per-card characterization

---

## 6. Open questions that gate the design

1. **Whose clock is in the FAT mtime?** (E1 — most important; everything else
   builds on this)
2. **Does SomnoPal have an NTP path while on the EZShare AP?** The EZShare AP
   has no upstream route. Options worth investigating (not solving here):
   dual-radio/AP+STA topologies, RTC holdover quality between syncs — and the
   elegant inversion: **SomnoTrace is on the home WiFi and already has NTP —
   it could push its clock *to* SomnoPal over the BLE link**, making SomnoPal
   a time *consumer* rather than an NTP client of its own. Also worth asking
   whether SomnoPal needs wall-clock *accuracy* at all, or just a stable
   monotonic reference that SomnoTrace's NTP-solved clock calibrates.
3. **What's the real append cadence and which files tick?** (E2)
4. **Buffered-write lag distribution** — how stale can on-disk data be vs the
   AS10's real-time state? Sets the floor on C4/C5 precision.
5. **Do AS10 file timestamps carry a timezone component?** Watch for a
   consistent hour-granularity offset separate from the true RTC drift —
   don't conflate TZ with drift. SomnoTrace already fights TZ on AS11
   (`as11_time.c`); expect the same battle here.
6. **Is the drift stable?** Characterize: constant offset, linear rate (ppm),
   steps (clock-set events), or RTC battery death jumps. The estimator must
   not assume continuity.
7. **FAT32 vs other formats** — mtime granularity and available time fields
   differ; 8.3 filenames limit what C6 can carry.
8. **Mid-write file reads** — reading a file while the AS10 is appending:
   how torn can a download be? Affects whether C5's tail-decode is reliable.
9. **Blind intervals** — how much of a session has observable writes? A
   machine that's idle writes nothing; estimate coverage.

---

## 7. What success looks like

Not a solution — a bar for whatever we build on top:

- A drift estimate with a **quantified error bound**, refreshed during a
  session, not just after the fact.
- Robust to **steps and rate changes**, not just a fixed offset fit.
- Explicit about **which channel supplied which pair** — some channels will
  prove noisy or version-dependent; the estimator must survive losing one.
- Cheap enough to run on the EZShare link's HTTP interface without hammering
  the card (poll rate is a real cost — the card's HTTP stack is weak).

---

## 8. Concrete experiments (on real hardware)

| # | Experiment | Teaches | Cheap? |
|---|---|---|---|
| E1 | Compare FAT mtime vs EDF-internal `starttime` for the same file | C1 — whose clock | ✅ first thing to run |
| E2 | Poll `/dir` at ~5 s during a live session; log full tree | C2/C3/C7 — event stream + beacon set | ✅ |
| E3 | Tail-download a growing EDF; diff bytes; decode annotations | C5 — content-event lag | medium |
| E4 | Capture HTTP response headers; check for `Date:` | C8 — EZShare clock | ✅ one curl |
| E5 | Log WiFi association state across a session boundary | C9 — power events | ✅ |
| E6 | Long-run drift: log same file's mtime-vs-NTP over days | drift *character* (offset/rate/steps) | medium |
| E7 | Dump raw card tree + EDF header corpus | C4/C6 — naming + header claims | ✅ |

E1 is the keystone: if FAT mtimes are AS10's clock, the problem is nearly
solved before it starts — every poll gives `mtime − ntp_now`. If they're not,
the real work shifts to C3/C5 (event-phase + content claims).

---

## 9. Explicit non-goals (for this document)

- ❌ Designing the estimator/fusion algorithm — that's a spec, after E1–E7.
- ❌ Timestamp-correction mechanics on ingest (a SomnoTrace-side concern).
- ❌ Any writes to the EZShare card. Full stop.
- ❌ Assuming the EZShare clock contributes — assume hostile until proven.
