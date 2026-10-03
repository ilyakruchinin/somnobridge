#!/usr/bin/env python3
# Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
# GPLv3+ — see LICENSE and NOTICE.

import argparse
import datetime as dt
import ftplib
import json
import math
import os
from pathlib import Path
import posixpath
import re
import select
import statistics
import tempfile
import time
import urllib.request
from zoneinfo import ZoneInfo

from clock_source import clock_bound, clock_source


class Evidence:
    def __init__(self, output=None):
        if output is None:
            output = Path(tempfile.mkdtemp(prefix="somnopal-validation-")) / "evidence.jsonl"
        self.path = Path(output)
        self.file = os.fdopen(os.open(self.path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w")

    def put(self, kind, **data):
        self.file.write(json.dumps({"kind": kind, "host_utc_ms": time.time_ns() // 1000000,
                                    "host_mono_ns": time.monotonic_ns(), **data}) + "\n")
        self.file.flush()

    def close(self):
        self.file.close()


def reference(host, source_uncertainty_ms, rounds=3):
    samples = []
    for i in range(rounds):
        request = urllib.request.Request(
            f"http://{host}/api/ble/passthrough",
            json.dumps({"id": int(time.time_ns() % 2147483647), "jsonrpc": "1.0",
                        "method": "GetDateTime"}).encode(),
            {"Content-Type": "application/json"})
        u1, m1 = time.time_ns() / 1e6, time.monotonic_ns()
        with urllib.request.urlopen(request, timeout=10) as response:
            body = response.read(65537)
        u2, m2 = time.time_ns() / 1e6, time.monotonic_ns()
        if len(body) > 65536:
            raise ValueError("oversized clock response")
        result = json.loads(body)
        raw = result.get("result", {}).get("dateTime")
        if not isinstance(raw, str):
            raise ValueError("GetDateTime failed")
        device = dt.datetime.fromisoformat(raw.replace("Z", "+00:00"))
        if device.utcoffset() is None:
            raise ValueError("clock response has no timezone")
        device_ms = device.timestamp() * 1000
        elapsed_ms = (m2 - m1) / 1e6
        stepped = abs((u2 - u1) - elapsed_ms) > 10
        quantization_ms = 1
        fraction = re.search(r"\.(\d+)", raw)
        if fraction is None:
            quantization_ms = 1000
        else:
            quantization_ms = 1000 * 10 ** -len(fraction.group(1))
        samples.append({"request_utc_ms": u1, "response_utc_ms": u2,
                        "request_mono_ns": m1, "response_mono_ns": m2,
                        "reported_device_ms": device_ms, "reported_resolution_ms": quantization_ms,
                        "resolution_verified": False, "rtt_ms": elapsed_ms,
                        "host_clock_step": stepped,
                        "drift_mid_ms": (u1 + u2) / 2 - device_ms,
                        "drift_interval_ms": [u1 - device_ms - source_uncertainty_ms - quantization_ms,
                                              u2 - device_ms + source_uncertainty_ms + quantization_ms]})
        if i + 1 < rounds:
            time.sleep(.2)
    return samples


def ble_connect(address):
    import dbus.mainloop.glib
    from ble_query import SomnoPal
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    dev = SomnoPal(address)
    try:
        dev.connect()
        dev.subscribe()
        return dev
    except Exception:
        dev.close()
        raise


def result(dev, op, args=None):
    response = dev.request(op, args, timeout=30)
    if not response.get("ok"):
        raise RuntimeError(op + ": " + str(response.get("error")))
    return response["result"]


def ftp_open(host):
    ftp = ftplib.FTP(host, timeout=20)
    ftp.login()
    ftp.voidcmd("TYPE I")
    return ftp


def remote_names(ftp, directory):
    return [posixpath.basename(x.rstrip("/")) for x in ftp.nlst(directory)]


def remote_json(ftp, path):
    buffer = bytearray()
    def receive(block):
        if len(buffer) + len(block) > 262144:
            raise ValueError("metadata exceeds 256 KiB")
        buffer.extend(block)
    ftp.retrbinary("RETR " + path, receive)
    return json.loads(buffer)


def edf_layout(prefix, length):
    if len(prefix) < 256:
        raise ValueError("short EDF header")
    h = int(prefix[184:192]); ns = int(prefix[252:256])
    if not 1 <= ns <= 63 or h != 256 * (1 + ns) or h > len(prefix):
        raise ValueError("invalid EDF layout")
    samples = [int(prefix[256 + 216 * ns + 8 * i:256 + 216 * ns + 8 * (i + 1)])
               for i in range(ns)]
    if any(x < 0 for x in samples):
        raise ValueError("negative EDF sample count")
    r = sum(samples) * 2
    duration = float(prefix[244:252])
    if r <= 0 or not math.isfinite(duration) or duration < 0 or length < h:
        raise ValueError("invalid record dimensions")
    local = dt.datetime.strptime(prefix[168:176].decode() + " " + prefix[176:184].decode(),
                                 "%d.%m.%y %H.%M.%S")
    return {"header_bytes": h, "record_bytes": r, "record_duration_s": duration,
            "signals": ns, "header_start_local": local.isoformat(),
            "declared_records": int(prefix[236:244]), "exact_bytes": length,
            "complete_records": (length - h) // r, "trailing_bytes": (length - h) % r}


def remote_edf(ftp, path):
    length = ftp.size(path)
    if length is None or length > 16 * 1024 * 1024:
        raise ValueError("missing length or EDF exceeds 16 MiB inspection cap")
    prefix = bytearray(); received = 0
    def receive(block):
        nonlocal received
        received += len(block)
        prefix.extend(block[:max(0, 16384 - len(prefix))])
    ftp.retrbinary("RETR " + path, receive)
    if received != length:
        raise ValueError("file changed during FTP read")
    return edf_layout(prefix, length)


def parse_stamp(raw):
    fields = re.fullmatch(r"\s*(\d{4})-\s*(\d+)-\s*(\d+)\s+(\d+):\s*(\d+):\s*(\d+)\s*", raw)
    if fields is None:
        raise ValueError("unrecognized listing timestamp")
    return dt.datetime(*map(int, fields.groups()))


def compare_history(files, sessions, timezone):
    rows = []
    for file in files:
        match = re.fullmatch(r"(\d{8}_\d{6})_(\w+)\.edf", file["name"], re.I)
        if not match or not file.get("ts"):
            continue
        local_start = dt.datetime.strptime(match.group(1), "%Y%m%d_%H%M%S")
        raw_start_ms = local_start.replace(tzinfo=timezone).timestamp() * 1000
        candidates = [s for s in sessions if s.get("clock_drift_valid") and s.get("clock_drift_usable")
                      and s.get("state") == "completed"]
        if not candidates:
            continue
        s = min(candidates, key=lambda s: abs(raw_start_ms + s["clock_drift_ms"] - s["start_epoch_ms"]))
        start_error = raw_start_ms + s["clock_drift_ms"] - s["start_epoch_ms"]
        if abs(start_error) > 120000:
            continue
        stamp = parse_stamp(file["ts"])
        row = {"name": file["name"], "session": s["id"], "type": match.group(2).upper(),
               "display_bytes": int(file["size"]), "name_start_error_ms": start_error,
               "drift_reference_ms": s["clock_drift_ms"],
               "stamp_as_utc_vs_end_ms": stamp.replace(tzinfo=dt.timezone.utc).timestamp() * 1000
                                         + s["clock_drift_ms"] - s["end_epoch_ms"],
               "stamp_as_local_vs_end_ms": stamp.replace(tzinfo=timezone).timestamp() * 1000
                                           + s["clock_drift_ms"] - s["end_epoch_ms"],
               "association": "filename_nearest_reference_start_not_header_verified"}
        rows.append(row)
    return rows


def history(args, evidence, bound):
    dev = ble_connect(args.address)
    try:
        status = result(dev, "GET_STATUS")
        catalog = result(dev, "LIST_DAY", {"day": args.day})
        evidence.put("status", result=status)
        evidence.put("catalog", result=catalog)
    finally:
        dev.close()
    sessions = []
    with ftp_open(args.host) as ftp:
        root = f"/.somnotrace/sessions/streams/{args.day}"
        for name in sorted(remote_names(ftp, root)):
            if not name.endswith("_session.json"):
                continue
            data = remote_json(ftp, posixpath.join(root, name))
            safe = {k: v for k, v in data.items() if k in {
                "id", "start_epoch_ms", "end_epoch_ms", "state", "brp_samples", "sa2_samples",
                "pld_samples", "gap_events", "gap_missing", "clock_drift_ms", "clock_drift_valid",
                "clock_drift_source", "clock_drift_measured_at_ms", "clock_drift_usable", "time_source"}}
            sessions.append(safe)
            evidence.put("session_reference", **safe)
        export_dir = f"/SDCARD/DATALOG/{args.day}"
        names = sorted(x for x in remote_names(ftp, export_dir) if x.lower().endswith(".edf"))
        for name in names[:args.exports]:
            layout = remote_edf(ftp, posixpath.join(export_dir, name))
            evidence.put("somnotrace_export_layout", name=name, **layout)
    rows = compare_history(catalog.get("files", []), sessions, ZoneInfo(args.timezone))
    for row in rows:
        evidence.put("historical_comparison", **row)
    samples = reference(args.host, bound["uncertainty_ms"])
    for sample in samples:
        evidence.put("clock_reference", **sample)
    best = min(samples, key=lambda x: x["rtt_ms"])
    print(json.dumps({"completed_reference_sessions": len(sessions), "native_catalog_files": len(catalog.get("files", [])),
                      "matched_catalog_files": len(rows), "live_reference_drift_ms": best["drift_mid_ms"],
                      "live_reference_interval_ms": best["drift_interval_ms"]}, indent=2), flush=True)
    for kind in sorted(set(r["type"] for r in rows)):
        selected = [r for r in rows if r["type"] == kind]
        print(json.dumps({"type": kind, "matches": len(selected),
                          "name_start_error_ms": [min(r["name_start_error_ms"] for r in selected),
                                                  max(r["name_start_error_ms"] for r in selected)],
                          "stamp_as_utc_vs_end_ms": [min(r["stamp_as_utc_vs_end_ms"] for r in selected),
                                                    max(r["stamp_as_utc_vs_end_ms"] for r in selected)]}), flush=True)


def serial_observations(fd, state, evidence):
    while select.select([fd], [], [], 0)[0]:
        block = os.read(fd, 8192)
        if not block:
            return
        state[0] += block
        while b"\n" in state[0]:
            line, state[0] = state[0].split(b"\n", 1)
            text = line.decode("utf-8", "replace")
            match = re.search(r"I \((\d+)\) sched: day (\d{8}): (\d+) listed, (.*)", text)
            if match:
                fields = dict(re.findall(r"(\w+)=([^\s,]+)", match[4]))
                changed = fields.get("chg", fields.get("changed", "0"))
                evidence.put("serial_day_result", device_log_ms=int(match[1]), day=match[2],
                             count=int(match[3]), changed=bool(int(changed)),
                             fields=fields,
                             timing_semantics="log_after_feed_not_http_issue")
            if "SomnoPal boot" in text or "Guru Meditation" in text:
                evidence.put("device_discontinuity", boot="SomnoPal boot" in text,
                             panic="Guru Meditation" in text)
        if len(state[0]) > 65536:
            state[0] = b""
            evidence.put("serial_overflow")


def capture(args, evidence, bound):
    dev = ble_connect(args.address)
    serial_fd = None
    serial_state = [b""]
    last_scan = None
    previous_files = {}
    end = time.monotonic() + args.duration
    next_snapshot = next_reference = next_anchor = 0
    snapshots = 0
    try:
        if args.serial:
            serial_fd = os.open(args.serial, os.O_RDONLY | os.O_NONBLOCK | os.O_NOCTTY)
        initial = result(dev, "GET_STATUS")
        evidence.put("capture_start", status=initial, duration_s=args.duration,
                     catalog_only=True, no_extra_card_polls=True)
        if not initial.get("sta_connected") or initial.get("budget", {}).get("backoff_ms"):
            raise RuntimeError("card link is not healthy")
        print("CAPTURE READY: passive card observations; therapy control is separate.", flush=True)
        while time.monotonic() < end:
            if serial_fd is not None:
                serial_observations(serial_fd, serial_state, evidence)
            now = time.monotonic()
            if now >= next_anchor:
                source = clock_source(args.source_uncertainty_ms,
                                      getattr(args, "source_epoch", None))
                mapping = dev.time_exchange(source=source, rounds=3)
                evidence.put("ble_anchor_exchange", result=mapping,
                             source=source)
                if source["uncertainty_ms"] > 100:
                    print("BLE timing captured for offline mapping; source uncertainty exceeds firmware's 100 ms anchor limit.", flush=True)
                else:
                    print("anchor exchange: " + ("committed" if mapping.get("committed") else mapping.get("why", "not committed")), flush=True)
                next_anchor = time.monotonic() + 600
            if now >= next_reference:
                bound = clock_bound(args.source_uncertainty_ms)
                evidence.put("clock_bound", **bound)
                for sample in reference(args.host, bound["uncertainty_ms"]):
                    evidence.put("clock_reference", **sample)
                next_reference = time.monotonic() + 60
            if now >= next_snapshot:
                u1, m1 = time.time_ns() // 1000000, time.monotonic_ns()
                status = result(dev, "GET_STATUS")
                catalog = result(dev, "LIST_DAY", {"day": args.day})
                scan_stamp = int(catalog.get("scan_age_us") or 0)
                evidence.put("catalog_snapshot", request_utc_ms=u1, request_mono_ns=m1,
                             status=status, result=catalog, device_scan_start_us=scan_stamp,
                             timing_semantics="legacy_scan_age_us_is_scan_start_monotonic_timestamp")
                fresh = last_scan is None or scan_stamp != last_scan
                if fresh:
                    current = {f["name"]: (f.get("size"), f.get("ts")) for f in catalog.get("files", [])}
                    changes = [{"name": k, "before": previous_files.get(k), "after": v}
                               for k, v in current.items() if k in previous_files and previous_files[k] != v]
                    evidence.put("observed_catalog_transition", changes=changes,
                                 added=[k for k in current if k not in previous_files], baseline=last_scan is None)
                    previous_files = current
                    last_scan = scan_stamp
                    snapshots += 1
                    therapy = status.get("therapy", {})
                    print(json.dumps({"scan": snapshots, "therapy_state": therapy.get("state"),
                                      "files": len(current), "changed_files": len(changes),
                                      "active_8h": status.get("budget", {}).get("active_8h")}), flush=True)
                budget = int(status.get("budget", {}).get("active_8h") or 0)
                if budget >= int(status.get("budget", {}).get("cap") or 700):
                    evidence.put("capture_stop", reason="budget_exhausted")
                    print("STOP: device request budget exhausted; no precision conclusion is possible.", flush=True)
                    break
                next_snapshot = time.monotonic() + args.interval
            dev._pump(.02)
    finally:
        if serial_fd is not None:
            os.close(serial_fd)
        dev.close()
        evidence.put("capture_end", fresh_catalog_views=snapshots)


SESSION_META_FIELDS = {
    "id", "state", "fmt", "start_epoch_ms", "end_epoch_ms", "start_iso",
    "end_iso", "brp_samples", "brp_mm_samples", "press_samples",
    "sa2_samples", "pld_samples", "stream_notifications", "gap_events",
    "gap_missing", "clock_drift_ms", "clock_drift_valid",
    "clock_drift_source", "clock_drift_measured_at_ms",
    "clock_drift_usable", "end_source_checkpoint_seq", "time_source"}


def observed_day(status, drift, fallback):
    phase = (status or {}).get("phase") or {}
    if re.fullmatch(r"\d{8}", str(phase.get("day") or "")):
        return phase["day"]
    estimate = (drift or {}).get("estimate") or {}
    for item in estimate.get("files") or []:
        text = "\\".join(str(item.get(k, "")) for k in ("dir", "name"))
        m = re.search(r"(?<!\d)(\d{8})(?!\d)", text)
        if m:
            return m.group(1)
    return fallback


def collect_session_references(host, day, evidence, revisions):
    """Whitelist timing/count metadata; never fetch stream or EDF content."""
    try:
        with ftp_open(host) as ftp:
            root = f"/.somnotrace/sessions/streams/{day}"
            names = sorted(remote_names(ftp, root))
            changed = 0
            for name in names:
                if not name.endswith("_session.json"):
                    continue
                try:
                    raw = remote_json(ftp, posixpath.join(root, name))
                except Exception as error:
                    evidence.put("session_reference_error", day=day,
                                 name=name, detail=str(error))
                    continue
                safe = {k: v for k, v in raw.items()
                        if k in SESSION_META_FIELDS}
                safe["record"] = name
                sig = json.dumps(safe, sort_keys=True, separators=(",", ":"))
                if revisions.get(name) != sig:
                    revisions[name] = sig
                    evidence.put("session_reference", day=day, **safe)
                    changed += 1
            return {"sessions_seen": len(revisions), "new_or_changed": changed}
    except Exception as error:
        evidence.put("session_reference_error", day=day, detail=str(error))
        return {"sessions_seen": len(revisions), "new_or_changed": 0,
                "error": str(error)}


JOURNAL_KEEP_KINDS = {"anchor", "timing_obs", "sot", "fallback",
                      "phase", "event"}


def collect_journal(dev, evidence):
    """Replay bounded timing evidence without storing unrelated records."""
    replay = dev.evidence(timeout=45)
    raw = replay.get("raw", b"")
    records = []
    malformed = skipped = 0
    for line in raw.decode("utf-8", "replace").splitlines():
        if not line.strip():
            continue
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            malformed += 1
            continue
        if rec.get("k") in JOURNAL_KEEP_KINDS:
            records.append(rec)
        else:
            skipped += 1
    evidence.put("journal_replay",
                 begin=(replay.get("begin") or {}).get("result"),
                 end=(replay.get("end") or {}).get("result"),
                 payload_bytes=len(raw), records=records,
                 malformed_records=malformed, skipped_records=skipped)
    return records


def observe(args, evidence, bound):
    """Passive multi-session collector: cached BLE reads + references only."""
    dev = ble_connect(args.address)
    serial_fd = None
    serial_state = [b""]
    session_revisions = {}
    snapshots = reconnects = 0
    status = drift = None
    end = time.monotonic() + args.duration
    next_snap = next_reference = next_anchor = next_meta = next_journal = 0
    try:
        if args.serial:
            serial_fd = os.open(args.serial, os.O_RDONLY | os.O_NONBLOCK |
                                os.O_NOCTTY)
        evidence.put("observe_start", duration_s=args.duration,
                     card_reads_added=False, therapy_control=False)
        print("OBSERVE READY: passive status/drift/reference collection.",
              flush=True)
        while time.monotonic() < end:
            if serial_fd is not None:
                serial_observations(serial_fd, serial_state, evidence)
            now = time.monotonic()
            if now >= next_snap:
                u1, m1 = time.time_ns() // 1000000, time.monotonic_ns()
                try:
                    status = result(dev, "GET_STATUS")
                    drift = result(dev, "GET_DRIFT",
                                   {"best_available": True})
                except Exception as error:
                    evidence.put("observer_error", stage="ble_poll",
                                 detail=str(error))
                    try:
                        dev.close()
                    except Exception:
                        pass
                    try:
                        dev = ble_connect(args.address)
                        reconnects += 1
                        evidence.put("observer_reconnect", count=reconnects)
                    except Exception as reconnect_error:
                        evidence.put("observer_error", stage="reconnect",
                                     detail=str(reconnect_error))
                    next_snap = time.monotonic() + 10
                    continue
                u2, m2 = time.time_ns() // 1000000, time.monotonic_ns()
                snapshots += 1
                evidence.put("somnopal_snapshot", request_utc_ms=u1,
                             request_mono_ns=m1, response_utc_ms=u2,
                             response_mono_ns=m2, status=status, drift=drift)
                therapy = (status.get("therapy") or {})
                estimate = (drift.get("estimate") or {})
                phase = (status.get("phase") or {})
                http = ((status.get("http") or {}).get("service_us") or {})
                active = therapy.get("state") == 1 or \
                    not estimate.get("observation_closed", True)
                print(json.dumps({
                    "snapshot": snapshots, "therapy_state": therapy.get("state"),
                    "half_ms": estimate.get("accuracy_half_ms"),
                    "windows": estimate.get("n_windows"),
                    "phase": phase.get("st"),
                    "http_n": http.get("n"),
                    "reconnects": reconnects}), flush=True)
                next_snap = now + (args.interval if active
                                   else args.idle_interval)
            if now >= next_reference:
                try:
                    source = clock_source(args.source_uncertainty_ms,
                                          args.source_epoch)
                    evidence.put("clock_source", **source)
                    for sample in reference(args.host,
                                            source["uncertainty_ms"]):
                        evidence.put("clock_reference", **sample)
                except Exception as error:
                    evidence.put("observer_error", stage="clock_reference",
                                 detail=str(error))
                next_reference = now + args.reference_interval
            if now >= next_anchor:
                try:
                    source = clock_source(args.source_uncertainty_ms,
                                          args.source_epoch)
                    mapping = dev.time_exchange(
                        source=source, rounds=args.anchor_rounds,
                        commit=not args.no_anchor_commit)
                    evidence.put("ble_anchor_exchange", result=mapping,
                                 source=source)
                except Exception as error:
                    evidence.put("observer_error", stage="ble_time_exchange",
                                 detail=str(error))
                next_anchor = now + args.anchor_interval
            day = observed_day(status, drift, args.day)
            if now >= next_meta:
                collect_session_references(args.host, day, evidence,
                                           session_revisions)
                next_meta = now + args.session_meta_interval
            if args.journal_interval and now >= next_journal:
                try:
                    collect_journal(dev, evidence)
                except Exception as error:
                    evidence.put("observer_error", stage="journal",
                                 detail=str(error))
                next_journal = now + args.journal_interval
            dev._pump(.02)
    finally:
        if serial_fd is not None:
            os.close(serial_fd)
        try:
            collect_session_references(args.host,
                                       observed_day(status, drift, args.day),
                                       evidence, session_revisions)
        except Exception:
            pass
        if args.journal_interval:
            try:
                collect_journal(dev, evidence)
            except Exception:
                pass
        dev.close()
        evidence.put("observe_end", snapshots=snapshots,
                     reconnects=reconnects,
                     session_metadata_revisions=len(session_revisions))


def read_capture(path):
    rows = []
    with open(path) as source:
        for line in source:
            if line.endswith("\n"):
                rows.append(json.loads(line))
    snapshots = [r for r in rows if r["kind"] == "catalog_snapshot"]
    if not snapshots:
        raise RuntimeError("capture has no catalog snapshot")
    return rows, snapshots[-1]


def control_rpc(host, method):
    if method not in {"EnterTherapy", "EnterStandby"}:
        raise ValueError("unsupported therapy control")
    request = urllib.request.Request(f"http://{host}/api/ble/passthrough",
        json.dumps({"id": int(time.time_ns() % 2147483647), "jsonrpc": "1.0", "method": method}).encode(),
        {"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=10) as response:
        data = json.loads(response.read(65536))
    if data.get("error") or "result" not in data:
        raise RuntimeError(method + " was rejected: " + str(data.get("error")))
    return data


def stream_sizes(host, day):
    root = f"/.somnotrace/sessions/streams/{day}"
    with ftp_open(host) as ftp:
        names = sorted(n for n in remote_names(ftp, root)
                       if n.lower().endswith(("_flow.snt", "_brp.snt")))[-3:]
        return {n: ftp.size(posixpath.join(root, n)) for n in names}


def capture_fresh(snapshot, max_age_s):
    age_ns = time.monotonic_ns() - snapshot["host_mono_ns"]
    return 0 <= age_ns <= max_age_s * 1000000000


def cycle(args, evidence):
    rows, initial = read_capture(args.input)
    status = initial["status"]
    therapy = status.get("therapy", {})
    if not capture_fresh(initial, 30) or any(r["kind"] == "capture_end" for r in rows):
        raise RuntimeError("capture is not current/running")
    if therapy.get("state") != 2 or therapy.get("stale") or not status.get("sta_connected"):
        raise RuntimeError("preflight requires healthy OUT_OF_THERAPY")
    cap = int(status.get("budget", {}).get("cap") or 700)
    if int(status.get("budget", {}).get("active_8h") or 0) >= cap - 20:
        raise RuntimeError("insufficient request budget for a controlled cycle")
    baseline = {f["name"] for f in initial["result"].get("files", [])}
    baseline_streams = stream_sizes(args.host, args.day)
    start_mono = time.monotonic()
    start_utc_ms = time.time_ns() / 1e6
    next_stream_check = start_mono + 45
    last_stream_progress = start_mono
    previous_stream_bytes = 0
    reason = "maximum_duration"
    try:
        evidence.put("therapy_start_requested", maximum_seconds=args.therapy_seconds)
        control_rpc(args.host, "EnterTherapy")
        evidence.put("therapy_start_acknowledged")
        print("EnterTherapy acknowledged; observing new files and updates.", flush=True)
        while time.monotonic() - start_mono < args.therapy_seconds:
            rows, latest = read_capture(args.input)
            status = latest["status"]
            growth = sum(any(c["name"] not in baseline and c["name"].upper().endswith("_BRP.EDF")
                             and c["before"][1] != c["after"][1] for c in r.get("changes", []))
                         for r in rows if r["kind"] == "observed_catalog_transition"
                         and r["host_utc_ms"] >= start_utc_ms)
            elapsed = time.monotonic() - start_mono
            if any(r["kind"] in {"capture_end", "device_discontinuity", "error"} for r in rows):
                reason = "capture_discontinuity"
                break
            if not capture_fresh(latest, 45):
                reason = "capture_stale"
                break
            if int(status.get("budget", {}).get("active_8h") or 0) >= cap - 12:
                reason = "stop_confirmation_budget_reserve"
                break
            polls = [r for r in rows if r["kind"] == "serial_day_result" and r["day"] == args.day]
            if not status.get("sta_connected") or (polls and not capture_fresh(polls[-1], 90)) or (not polls and elapsed >= 90):
                reason = "card_observation_unhealthy"
                break
            if time.monotonic() >= next_stream_check:
                sizes = stream_sizes(args.host, args.day)
                current_bytes = sum(size or 0 for name, size in sizes.items() if name not in baseline_streams)
                evidence.put("stream_progress", new_stream_bytes=current_bytes,
                             filesystem_therapy_state=status.get("therapy", {}).get("state"))
                if current_bytes > previous_stream_bytes:
                    last_stream_progress = time.monotonic()
                    previous_stream_bytes = current_bytes
                next_stream_check = time.monotonic() + 60
            if time.monotonic() - last_stream_progress >= 180:
                reason = "no_somnotrace_stream_progress"
                break
            if elapsed >= 240 and growth >= 5:
                reason = "five_brp_timestamp_updates_observed"
                break
            time.sleep(5)
        evidence.put("therapy_stop_reason", reason=reason)
    finally:
        stopped = False
        for attempt in range(3):
            try:
                control_rpc(args.host, "EnterStandby")
                evidence.put("therapy_stop_acknowledged", attempt=attempt + 1, reason=reason)
                print("EnterStandby acknowledged; capture continues for final writes/idle confirmation.", flush=True)
                stopped = True
                break
            except Exception as error:
                evidence.put("therapy_stop_error", attempt=attempt + 1, detail=str(error))
                time.sleep(1)
        if not stopped:
            raise RuntimeError("STOP RPC FAILED: physically stop therapy and check the device")


def interval_relation(estimate, reference):
    """Classify interval coverage; both arguments are [low, high]."""
    if not estimate or not reference:
        return "unavailable"
    if estimate[0] > estimate[1] or reference[0] > reference[1]:
        return "invalid"
    if reference[0] >= estimate[0] and reference[1] <= estimate[1]:
        return "reference_contained"
    if estimate[0] >= reference[0] and estimate[1] <= reference[1]:
        return "estimate_contained"
    if estimate[1] >= reference[0] and reference[1] >= estimate[0]:
        return "overlap"
    return "disjoint"


def expand_time_reference(reference, at_ms, rate_ppm):
    """Move a correction interval to another instant using a rate bound."""
    if not reference:
        return None
    dt_s = abs(at_ms - reference["response_utc_ms"]) / 1000.0
    grow_ms = dt_s * rate_ppm / 1000.0
    lo, hi = reference["drift_interval_ms"]
    return [lo - grow_ms, hi + grow_ms]


def nearest_reference(references, at_ms):
    usable = [r for r in references
              if isinstance(r.get("drift_interval_ms"), list) and
              not r.get("host_clock_step")]
    if not usable:
        return None
    return min(usable,
               key=lambda r: abs(r.get("response_utc_ms", 0) - at_ms))


def ble_anchor_points(rows):
    anchors = []
    for row in rows:
        if row["kind"] != "ble_anchor_exchange":
            continue
        result = row.get("result") or {}
        source = result.get("source") or row.get("source") or {}
        for ex in result.get("exchanges") or []:
            if not ex.get("ok") or ex.get("host_clock_step"):
                continue
            anchors.append({
                "mono_us": (ex["m2"] + ex["m3"]) / 2,
                "utc_us": (ex["t1"] + ex["t4"]) / 2,
                "unc_us": ex["unc_us"],
                "source_uncertainty_us": ex.get("source_uncertainty_us"),
                "source_epoch": ex.get("epoch"),
                "source": source,
            })
    anchors.sort(key=lambda a: a["mono_us"])
    return anchors


def map_with_anchor(obs_lo_us, obs_hi_us, anchors, rate_ppm=500):
    """Conservative nearest-anchor map; no interpolation across domains."""
    if not anchors:
        return None
    mid = (obs_lo_us + obs_hi_us) / 2
    anchor = min(anchors, key=lambda a: abs(a["mono_us"] - mid))
    d_lo = obs_lo_us - anchor["mono_us"]
    d_hi = obs_hi_us - anchor["mono_us"]
    pad_lo = abs(d_lo) * rate_ppm / 1000000.0
    pad_hi = abs(d_hi) * rate_ppm / 1000000.0
    return [(anchor["utc_us"] - anchor["unc_us"] + d_lo - pad_lo) / 1000.0,
            (anchor["utc_us"] + anchor["unc_us"] + d_hi + pad_hi) / 1000.0]


def unique_observations(rows):
    observations = {}
    for row in rows:
        if row["kind"] == "somnopal_snapshot":
            estimate = ((row.get("drift") or {}).get("estimate") or {})
            files = estimate.get("files") or []
            beacon = files[0].get("name") if files else None
            day_match = re.search(r"(\d{8})",
                                  files[0].get("dir", "")) if files else None
            beacon_day = day_match.group(1) if day_match else \
                (beacon[:8] if beacon else None)
            session = estimate.get("session_start_as10")
            model = estimate.get("timing_model") or {}
            for obs in estimate.get("recent_observations") or []:
                key = (beacon_day, beacon, obs.get("seq"))
                merged = dict(obs)
                merged.update({"session_start_as10": session,
                               "day": beacon_day,
                               "beacon": beacon,
                               "timing_model": model,
                               "observation_source": "get_drift"})
                observations[key] = merged
        elif row["kind"] == "journal_replay":
            records = row.get("records") or []
            boot_i = None
            for i, rec in enumerate(records):
                if rec.get("k") == "event" and rec.get("e") == "boot":
                    boot_i = i
            for i, rec in enumerate(records):
                if rec.get("k") != "timing_obs":
                    continue
                if boot_i is not None and i < boot_i:
                    continue
                obs_us = rec.get("obs_us") or []
                if len(obs_us) != 2:
                    continue
                unchanged_service = rec.get("unchanged_service_us")
                changed_service = rec.get("changed_service_us")
                scans = rec.get("scans") or [None, None]
                obs = {
                    "seq": rec.get("seq"), "kind": rec.get("kind"),
                    "ev": rec.get("ev"), "unch_seq": scans[0],
                    "chg_seq": scans[1], "stamp_s": rec.get("stamp_s"),
                    "unch_req_us": obs_us[0],
                    "unch_resp_us": (obs_us[0] + unchanged_service
                                     if unchanged_service is not None else None),
                    "chg_req_us": (obs_us[1] - changed_service
                                   if changed_service is not None else None),
                    "chg_resp_us": obs_us[1],
                    "observation_source": "journal",
                    "day": rec.get("day"), "beacon": rec.get("beacon"),
                }
                if rec.get("utc_ms"):
                    obs["visibility_utc_ms"] = rec["utc_ms"]
                    m = obs["stamp_s"] * 1000
                    obs["offset_interval_ms"] = [
                        rec["utc_ms"][0] - (m + 2000),
                        rec["utc_ms"][1] - m]
                key = (rec.get("day"), rec.get("beacon"), rec.get("seq"))
                observations.setdefault(key, obs)
    return list(observations.values())


def journal_preboot_observations(rows):
    count = 0
    for row in rows:
        if row["kind"] != "journal_replay":
            continue
        records = row.get("records") or []
        boot_i = None
        for i, rec in enumerate(records):
            if rec.get("k") == "event" and rec.get("e") == "boot":
                boot_i = i
        if boot_i is None:
            continue
        count += sum(1 for i, rec in enumerate(records)
                     if i < boot_i and rec.get("k") == "timing_obs")
    return count


def score_update5(rows):
    references = [r for r in rows if r["kind"] == "clock_reference"]
    anchors = ble_anchor_points(rows)
    observations = unique_observations(rows)
    delays = []
    offset_relations = {}
    residuals_ms = []
    for obs in observations:
        stamp_s = obs.get("stamp_s")
        lo_us = obs.get("unch_req_us", obs.get("unchanged_req_us"))
        hi_us = obs.get("chg_resp_us", obs.get("changed_resp_us"))
        if stamp_s is None or lo_us is None or hi_us is None:
            continue
        model = obs.get("timing_model") or {}
        rate_ppm = model.get("dev_rate_ppm") or 500
        mapped = map_with_anchor(lo_us, hi_us, anchors, rate_ppm)
        visibility = mapped or obs.get("visibility_utc_ms")
        visibility_source = "ble_exchange" if mapped else \
            "device_anchor" if visibility else "unavailable"
        if not visibility:
            continue
        vis_mid = sum(visibility) / 2
        ref = nearest_reference(references, vis_mid)
        ref_iv = expand_time_reference(ref, vis_mid, rate_ppm)
        stamp_ms = int(stamp_s) * 1000
        delay = None
        if ref_iv:
            stamp_iv = [stamp_ms + ref_iv[0],
                        stamp_ms + 2000 + ref_iv[1]]
            delay = [visibility[0] - stamp_iv[1],
                     visibility[1] - stamp_iv[0]]
            delays.append({"seq": obs.get("seq"), "kind": obs.get("kind"),
                           "delay_ms": delay,
                           "visibility_source": visibility_source,
                           "reference_age_ms": abs(
                               vis_mid - ref["response_utc_ms"]),
                           "reference_rtt_ms": ref.get("rtt_ms")})
        est_iv = obs.get("offset_interval_ms")
        relation = interval_relation(est_iv, ref_iv)
        offset_relations[relation] = offset_relations.get(relation, 0) + 1
        if est_iv and ref_iv:
            residuals_ms.append((sum(est_iv) / 2) - (sum(ref_iv) / 2))

    estimate_scores = []
    seen_estimate_scores = set()
    for row in rows:
        if row["kind"] != "somnopal_snapshot":
            continue
        estimate = ((row.get("drift") or {}).get("estimate") or {})
        interval = estimate.get("drift_interval_ms")
        if not interval:
            continue
        at = estimate.get("reference_at_ms") or row["host_utc_ms"]
        model = estimate.get("timing_model") or {}
        rate_ppm = model.get("dev_rate_ppm") or 500
        ref = nearest_reference(references, at)
        ref_iv = expand_time_reference(ref, at, rate_ppm)
        score = {"at_ms": at, "estimate_ms": interval,
                 "half_ms": estimate.get("accuracy_half_ms"),
                 "timing_model": model,
                 "relation": interval_relation(interval, ref_iv)}
        if ref_iv:
            score["reference_ms"] = ref_iv
            score["midpoint_delta_ms"] = sum(interval) / 2 - sum(ref_iv) / 2
            score["reference_age_ms"] = abs(at - ref["response_utc_ms"])
        key = (tuple(interval), at,
               tuple(ref_iv) if ref_iv else None)
        if key not in seen_estimate_scores:
            seen_estimate_scores.add(key)
            estimate_scores.append(score)

    session_scores = []
    estimates = [s for s in estimate_scores if s.get("estimate_ms")]
    latest = estimates[-1] if estimates else None
    for row in rows:
        if row["kind"] != "session_reference" or not latest:
            continue
        if not row.get("clock_drift_valid") or not row.get("clock_drift_usable"):
            continue
        point = row.get("clock_drift_ms")
        at = row.get("clock_drift_measured_at_ms") or row.get("end_epoch_ms")
        if point is None or at is None:
            continue
        model = (((latest or {}).get("timing_model")) or {})
        rate_ppm = model.get("dev_rate_ppm") or 500
        grow = abs(at - latest["at_ms"]) * rate_ppm / 1000000.0
        ref_iv = [point - grow, point + grow]
        session_scores.append({
            "record": row.get("record"), "point_ms": point,
            "at_ms": at, "estimate_ms": latest["estimate_ms"],
            "relation": interval_relation(latest["estimate_ms"], ref_iv),
            "midpoint_delta_ms": sum(latest["estimate_ms"]) / 2 - point,
            "reference_age_ms": abs(at - latest["at_ms"])})

    delay_values = [d["delay_ms"] for d in delays]
    summary = {
        "observations": len(observations),
        "journal_preboot_observations": journal_preboot_observations(rows),
        "ble_anchor_exchanges": len(anchors),
        "clock_references": len(references),
        "delay_intervals": delays,
        "offset_relations": offset_relations,
        "offset_midpoint_delta_ms": {
            "min": min(residuals_ms), "max": max(residuals_ms),
            "median": statistics.median(residuals_ms)} if residuals_ms else None,
        "latest_estimate_score": latest,
        "estimate_scores": estimate_scores[-20:],
        "session_reference_scores": session_scores,
    }
    if delay_values:
        summary["delay_hull_ms"] = [min(d[0] for d in delay_values),
                                    max(d[1] for d in delay_values)]
        summary["delay_containing_zero"] = sum(d[0] <= 0 <= d[1]
                                               for d in delay_values)
        summary["delay_entirely_positive"] = sum(d[0] > 0
                                                 for d in delay_values)
        summary["delay_entirely_negative"] = sum(d[1] < 0
                                                 for d in delay_values)
        fresh = [d["delay_ms"] for d in delays
                 if d.get("reference_age_ms") is not None and
                 d["reference_age_ms"] <= 120000]
        if fresh:
            summary["delay_hull_fresh_ms"] = [
                min(d[0] for d in fresh), max(d[1] for d in fresh)]
            summary["fresh_delay_samples"] = len(fresh)
    return summary


def analyze(path):
    counts = {}
    samples = []
    transitions = {}
    serial_polls = 0
    states = set()
    rows = []
    with open(path) as source:
        for line in source:
            row = json.loads(line)
            rows.append(row)
            kind = row["kind"]
            counts[kind] = counts.get(kind, 0) + 1
            if kind == "clock_reference" and not row["host_clock_step"]:
                samples.append(row)
            if kind == "serial_day_result":
                serial_polls += 1
            if kind == "catalog_snapshot" or kind == "somnopal_snapshot":
                status = row.get("status") or {}
                states.add(status.get("therapy", {}).get("state"))
            if kind == "observed_catalog_transition":
                for change in row["changes"]:
                    before, after = change["before"], change["after"]
                    file_type = change["name"].rsplit("_", 1)[-1].lower()
                    entry = transitions.setdefault(file_type, {"size_only": 0, "stamp_only": 0,
                                                                "both": 0, "stamp_delta_s": []})
                    size_changed, stamp_changed = before[0] != after[0], before[1] != after[1]
                    entry["both" if size_changed and stamp_changed else "size_only" if size_changed else "stamp_only"] += 1
                    if stamp_changed and before[1] and after[1]:
                        entry["stamp_delta_s"].append((parse_stamp(after[1]) - parse_stamp(before[1])).total_seconds())
    summary = {"records": counts, "serial_day_results": serial_polls, "therapy_states_seen": sorted(states),
               "changes_by_type": transitions,
               "precision_note": "cached observations and post-feed serial logs do not establish exact write latency"}
    if samples:
        summary["reference"] = {"samples": len(samples),
                                "median_drift_ms": statistics.median(s["drift_mid_ms"] for s in samples),
                                "minimum_rtt_ms": min(s["rtt_ms"] for s in samples),
                                "interval_envelope_ms": [min(s["drift_interval_ms"][0] for s in samples),
                                                         max(s["drift_interval_ms"][1] for s in samples)]}
    summary["update5_scoring"] = score_update5(rows)
    print(json.dumps(summary, indent=2))


def self_test():
    assert capture_fresh({"host_mono_ns": time.monotonic_ns() - 5000000000}, 30)
    assert not capture_fresh({"host_mono_ns": time.monotonic_ns() - 31000000000}, 30)
    assert not capture_fresh({"host_mono_ns": time.monotonic_ns() + 1000000000}, 30)
    from types import SimpleNamespace
    from unittest.mock import Mock, patch
    snapshot = {"kind": "catalog_snapshot", "host_mono_ns": time.monotonic_ns(),
                "status": {"therapy": {"state": 2, "stale": False}, "sta_connected": True,
                           "budget": {"active_8h": "10", "cap": 700}}, "result": {"files": []}}
    commands = []
    def fake_control(host, method):
        commands.append(method)
        if method == "EnterTherapy":
            raise RuntimeError("synthetic start transport failure")
        return {"result": None}
    with patch(__name__ + ".read_capture", return_value=([snapshot], snapshot)), \
         patch(__name__ + ".stream_sizes", return_value={}), \
         patch(__name__ + ".control_rpc", side_effect=fake_control):
        try:
            cycle(SimpleNamespace(input="synthetic", host="synthetic", day="20260101", therapy_seconds=240), Mock())
        except RuntimeError as error:
            assert str(error) == "synthetic start transport failure"
        else:
            raise AssertionError("failed start must propagate")
    assert commands == ["EnterTherapy", "EnterStandby"]
    h = bytearray(b" " * 768)
    h[168:176] = b"01.01.26"; h[176:184] = b"00.00.00"
    h[184:192] = b"768     "; h[236:244] = b"2       "
    h[244:252] = b"60      "; h[252:256] = b"2   "
    h[688:696] = b"10      "; h[696:704] = b"20      "
    assert edf_layout(h, 888)["complete_records"] == 2
    assert edf_layout(h, 889)["trailing_bytes"] == 1
    assert parse_stamp("2026- 1- 1  0: 0: 2") == dt.datetime(2026, 1, 1, 0, 0, 2)
    h[244:252] = b"0.00    "
    assert edf_layout(h, 888)["record_duration_s"] == 0
    zone = ZoneInfo("Australia/Melbourne")
    raw_ms = dt.datetime(2026, 1, 1, tzinfo=zone).timestamp() * 1000
    s = {"id": "synthetic", "state": "completed", "clock_drift_valid": True,
         "clock_drift_usable": True, "clock_drift_ms": -500000,
         "start_epoch_ms": raw_ms - 500000, "end_epoch_ms": raw_ms - 440000}
    rows = compare_history([{"name": "20260101_000000_BRP.edf", "size": "1024",
                             "ts": "2025-12-31 13:01:00"}], [s], zone)
    assert len(rows) == 1 and rows[0]["stamp_as_utc_vs_end_ms"] == 0
    s["clock_drift_valid"] = False
    assert not compare_history([{"name": "20260101_000000_BRP.edf", "size": "1024",
                                 "ts": "2025-12-31 13:01:00"}], [s], zone)
    rows = [
        {"kind": "ble_anchor_exchange", "result": {"source": {"epoch": 7},
            "exchanges": [{"ok": True, "m2": 10000000, "m3": 10000000,
                           "t1": 1000500000, "t4": 1000500000,
                           "unc_us": 50000, "host_clock_step": False,
                           "source_uncertainty_us": 40000, "epoch": 7}]}},
        {"kind": "clock_reference", "response_utc_ms": 1000500,
         "rtt_ms": 50, "drift_interval_ms": [80, 120],
         "host_clock_step": False},
        {"kind": "somnopal_snapshot", "host_utc_ms": 1001000,
         "drift": {"estimate": {"session_start_as10": "synthetic",
             "timing_model": {"dev_rate_ppm": 500},
             "drift_interval_ms": [-1500, 700],
             "accuracy_half_ms": 1100,
             "reference_at_ms": 1000500,
             "recent_observations": [
                 {"seq": 1, "kind": "transition", "stamp_s": 1000,
                  "unch_req_us": 10000000, "chg_resp_us": 10000500,
                  "visibility_utc_ms": [1000500, 1000700],
                  "offset_interval_ms": [-1500, 700]}]}}}]
    rows.append({"kind": "journal_replay", "records": [
        {"k": "timing_obs", "t": 1000, "seq": 99, "stamp_s": 999,
         "obs_us": [1, 2]},
        {"k": "event", "e": "boot", "t": 2000}]})
    score = score_update5(rows)
    assert score["observations"] == 1 and score["ble_anchor_exchanges"] == 1
    assert score["journal_preboot_observations"] == 1
    assert score["delay_containing_zero"] == 1
    assert score["latest_estimate_score"]["relation"] == "reference_contained"
    print("synthetic self-tests passed")


def main():
    parser = argparse.ArgumentParser(description="SomnoPal / SomnoTrace timing validation; explicit cycle mode controls therapy")
    parser.add_argument("mode", choices=["history", "capture", "observe", "cycle",
                                       "analyze", "self-test"])
    parser.add_argument("--host", default="192.168.86.102")
    parser.add_argument("--address", default="8C:FD:49:93:74:76")
    parser.add_argument("--day", default="20260930")
    parser.add_argument("--timezone", default="Australia/Melbourne")
    parser.add_argument("--source-uncertainty-ms", type=float)
    parser.add_argument("--source-epoch", type=lambda x: int(x, 0),
                        help="explicit host time-source epoch for anchors")
    parser.add_argument("--exports", type=int, default=5)
    parser.add_argument("--duration", type=float, default=1200)
    parser.add_argument("--interval", type=float, default=5,
                        help="active-session observation interval")
    parser.add_argument("--idle-interval", type=float, default=30,
                        help="observer poll interval while out of therapy")
    parser.add_argument("--reference-interval", type=float, default=60,
                        help="AS11 GetDateTime reference interval")
    parser.add_argument("--anchor-interval", type=float, default=600,
                        help="BLE host/device mapping interval")
    parser.add_argument("--anchor-rounds", type=int, default=3)
    parser.add_argument("--no-anchor-commit", action="store_true",
                        help="keep TIME exchanges host-side only")
    parser.add_argument("--session-meta-interval", type=float, default=60)
    parser.add_argument("--journal-interval", type=float, default=600,
                        help="seconds between bounded journal replays; 0 disables")
    parser.add_argument("--serial", default="/dev/ttyACM0")
    parser.add_argument("--output")
    parser.add_argument("--input")
    parser.add_argument("--therapy-seconds", type=float, default=480)
    parser.add_argument("--allow-therapy-control", action="store_true")
    args = parser.parse_args()
    if args.mode == "cycle":
        if not args.input or not args.allow_therapy_control or not 240 <= args.therapy_seconds <= 600:
            parser.error("cycle requires --input, --allow-therapy-control, and therapy-seconds 240..600")
        evidence = Evidence(args.output)
        print("private control evidence: " + str(evidence.path), flush=True)
        try:
            cycle(args, evidence)
        finally:
            evidence.close()
        return
    if args.mode == "analyze":
        if not args.input:
            parser.error("analyze requires --input")
        analyze(args.input)
        return
    if args.mode == "self-test":
        self_test()
        return
    if not (0 <= args.exports <= 25 and 1 <= args.duration <= 7200 and
            2 <= args.interval <= 60 and 5 <= args.idle_interval <= 600 and
            5 <= args.reference_interval <= 3600 and
            30 <= args.anchor_interval <= 3600 and
            1 <= args.anchor_rounds <= 10 and
            10 <= args.session_meta_interval <= 3600 and
            0 <= args.journal_interval <= 3600):
        parser.error("exports 0..25, duration 1..7200s, active interval 2..60s, "
                     "idle/reference/session intervals within bounds, "
                     "anchor rounds 1..10, journal interval 0..3600s")
    evidence = Evidence(args.output)
    try:
        bound = clock_bound(args.source_uncertainty_ms)
        evidence.put("clock_bound", **bound)
        print("private evidence: " + str(evidence.path), flush=True)
        print("host source uncertainty: ±" + str(bound["uncertainty_ms"]) + " ms", flush=True)
        if args.mode == "history":
            history(args, evidence, bound)
        elif args.mode == "observe":
            observe(args, evidence, bound)
        else:
            capture(args, evidence, bound)
    except KeyboardInterrupt:
        evidence.put("interrupted")
    except Exception as error:
        evidence.put("error", error_type=type(error).__name__, detail=str(error))
        raise
    finally:
        evidence.close()


if __name__ == "__main__":
    main()
