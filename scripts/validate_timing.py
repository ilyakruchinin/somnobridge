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
import subprocess
import tempfile
import time
import urllib.request
from zoneinfo import ZoneInfo


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


def clock_bound(override=None):
    if override is not None:
        if not math.isfinite(override) or override <= 0:
            raise ValueError("source uncertainty must be finite and positive")
        return {"source": "explicit", "uncertainty_ms": override}
    report = subprocess.check_output(["chronyc", "tracking"], text=True, timeout=5)
    def number(label):
        m = re.search(r"^" + re.escape(label) + r"\s*:\s*([-\d.]+)", report, re.M)
        if not m:
            raise ValueError("missing chrony field: " + label)
        return abs(float(m.group(1)))
    if not re.search(r"Leap status\s*:\s*Normal", report):
        raise ValueError("chrony is not synchronized")
    bound = (number("Root delay") / 2 + number("Root dispersion") + number("System time")) * 1000
    return {"source": "chrony_root_distance", "uncertainty_ms": math.ceil(bound + 5),
            "rms_offset_ms": number("RMS offset") * 1000}


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
            match = re.search(r"I \((\d+)\) sched: day (\d{8}): (\d+) listed, changed=(\d) grown=(\S+)", text)
            if match:
                evidence.put("serial_day_result", device_log_ms=int(match[1]), day=match[2],
                             count=int(match[3]), changed=bool(int(match[4])), grown=match[5],
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
                mapping = dev.time_exchange(unc_us=math.ceil(bound["uncertainty_ms"] * 1000), rounds=3)
                evidence.put("ble_anchor_exchange", result=mapping, source_uncertainty_ms=bound["uncertainty_ms"])
                if bound["uncertainty_ms"] > 100:
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


def analyze(path):
    counts = {}
    samples = []
    transitions = {}
    serial_polls = 0
    states = set()
    with open(path) as source:
        for line in source:
            row = json.loads(line)
            kind = row["kind"]
            counts[kind] = counts.get(kind, 0) + 1
            if kind == "clock_reference" and not row["host_clock_step"]:
                samples.append(row)
            if kind == "serial_day_result":
                serial_polls += 1
            if kind == "catalog_snapshot":
                states.add(row["status"].get("therapy", {}).get("state"))
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
    print("synthetic self-tests passed")


def main():
    parser = argparse.ArgumentParser(description="SomnoPal / SomnoTrace timing validation; explicit cycle mode controls therapy")
    parser.add_argument("mode", choices=["history", "capture", "cycle", "analyze", "self-test"])
    parser.add_argument("--host", default="192.168.86.102")
    parser.add_argument("--address", default="8C:FD:49:93:74:76")
    parser.add_argument("--day", default="20260930")
    parser.add_argument("--timezone", default="Australia/Melbourne")
    parser.add_argument("--source-uncertainty-ms", type=float)
    parser.add_argument("--exports", type=int, default=5)
    parser.add_argument("--duration", type=float, default=1200)
    parser.add_argument("--interval", type=float, default=5)
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
    if not 0 <= args.exports <= 25 or not 1 <= args.duration <= 7200 or not 2 <= args.interval <= 60:
        parser.error("exports must be 0..25, duration 1..7200s, interval 2..60s")
    evidence = Evidence(args.output)
    try:
        bound = clock_bound(args.source_uncertainty_ms)
        evidence.put("clock_bound", **bound)
        print("private evidence: " + str(evidence.path), flush=True)
        print("host source uncertainty: ±" + str(bound["uncertainty_ms"]) + " ms", flush=True)
        if args.mode == "history":
            history(args, evidence, bound)
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
