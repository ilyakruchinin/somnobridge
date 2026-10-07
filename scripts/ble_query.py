#!/usr/bin/env python3
# SomnoBridge — host-side BLE query tool (status / catalog / drift / time sync)
# Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
# GPLv3+ — see LICENSE and NOTICE.
#
# Talks to a bonded SomnoBridge over BlueZ's D-Bus API.
#
# Why D-Bus and not bluetoothctl: this bluez build's CLI `write` command
# returns org.bluez.Error.NotSupported for every variant and never reaches
# the device.  org.bluez.GattCharacteristic1.WriteValue works — it is what
# SomnoTrace uses.
#
# Examples:
#   ./scripts/ble_query.py                       # status+catalog+days+drift
#   ./scripts/ble_query.py --time-sync 5         # run+commit a §8.1 anchor
#   ./scripts/ble_query.py --days 14 --day 20240103
#   ./scripts/ble_query.py --drift               # estimate + SOT envelope
#   ./scripts/ble_query.py --evidence            # replay the evidence journal
#   ./scripts/ble_query.py --json                # raw JSON instead of tables

import argparse
import hashlib
import json
import math
import re
import struct
import sys
import time
import zlib

import dbus
import dbus.mainloop.glib
from gi.repository import GLib

try:
    import numpy as _np
except ImportError:
    _np = None

from clock_source import clock_source

SVC_UUID = "8ba7d100-6e4b-4bb4-9f1d-7306766e6f50"
CTRL_UUID = "8ba7d101-6e4b-4bb4-9f1d-7306766e6f50"
DATA_UUID = "8ba7d102-6e4b-4bb4-9f1d-7306766e6f50"

# blep_kind_t (components/ble_api/include/ble_proto.h)
K_JSON_REQ, K_JSON_RESP, K_FILE_BYTES, K_ACK, K_ABORT, \
    K_TIME_REQ, K_TIME_REPLY, K_TIME_COMMIT, K_EV_BYTES, \
    K_STREAM_DATA = range(1, 11)

FLAG_START, FLAG_END = 0x0001, 0x0002
HDR = struct.Struct("<BBHIQ")           # 16-byte packed LE header
HDR_LEN = HDR.size
CHDR_LEN = 4                            # compact data frame header
BLK_HDR = struct.Struct("<BBHII")       # stream block envelope (12 B)

STREAM_WINDOW = 48 * 240                # credit grant, wire bytes

DEFAULT_ADDR = "8C:FD:49:93:74:76"
TIMESTAMP_ALLOWANCE_US = 5000           # §8.1 provisional 5 ms
RTT_MAX_US = 250000
SRC_QUAL_UNC_MAX_US = 100000
SRC_UNC_MAX_US = 500000


def frame(kind, rid, offset, flags, payload=b""):
    return HDR.pack(1, kind, flags, rid, offset) + payload


class SomnoBridge:
    """Minimal BlueZ GATT client for the SomnoBridge vendor service."""

    def __init__(self, address, verbose=False):
        self.addr = address.upper()
        self.verbose = verbose
        self.bus = dbus.SystemBus()
        self.dev_path = None
        self.ctrl_path = self.data_path = None
        self._ctrl = self._data = None
        self._rx = {}                   # rid -> bytearray (JSON reassembly)
        self._rx_poisoned = set()       # rids with a lost fragment
        self._done = {}                 # rid -> decoded JSON string
        self._time_reply = {}           # rid -> raw TIME_REPLY payload
        self._stream = bytearray()      # evidence/file wire bytes
        self._stream_end = None         # STREAM_END JSON
        self._stream_rid = None
        self._stream_bytes = 0
        self._stream_open = False
        self._stream_seq = 0            # next expected compact seq
        self._seq_errors = 0
        self._stream_frames = 0         # compact frames received
        self._dup_frames = 0            # duplicate deliveries dropped
        self._last_acked = 0            # last right-edge credit grant
        self._ack_pending = False
        # rotate the request_id base per run so a retry never collides
        # with a still-cached exchange from a previous invocation
        self._rid = int(time.time_ns() // 1000) & 0xFFFFF0

    # ---------- plumbing ----------

    def log(self, *a):
        if self.verbose:
            print("[ble]", *a, file=sys.stderr)

    def _pump(self, seconds):
        """Dispatch queued D-Bus signals for up to `seconds`."""
        ctx = GLib.MainContext.default()
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            while ctx.pending():
                ctx.iteration(False)
            time.sleep(0.002)

    def _managed(self):
        om = dbus.Interface(self.bus.get_object("org.bluez", "/"),
                            "org.freedesktop.DBus.ObjectManager")
        return om.GetManagedObjects()

    def _props(self, path):
        return dbus.Interface(self.bus.get_object("org.bluez", path),
                              "org.freedesktop.DBus.Properties")

    def _get(self, path, iface, name, default=None):
        try:
            return self._props(path).Get(iface, name)
        except dbus.exceptions.DBusException:
            return default

    def _find(self, objects, iface, uuid):
        for path, ifaces in objects.items():
            if iface in ifaces and \
               str(ifaces[iface].get("UUID", "")).lower() == uuid:
                return path
        return None

    # ---------- connect / subscribe ----------

    def connect(self, timeout=20.0):
        objects = self._managed()
        for path, ifaces in objects.items():
            d = ifaces.get("org.bluez.Device1")
            if d and str(d.get("Address", "")).upper() == self.addr:
                self.dev_path = path
                break
        if not self.dev_path:
            raise RuntimeError(
                f"{self.addr} not known to BlueZ — scan first "
                f"(bluetoothctl scan le) or check the address")

        if not self._get(self.dev_path, "org.bluez.Device1", "Connected"):
            self.log("connecting...")
            dbus.Interface(self.bus.get_object("org.bluez", self.dev_path),
                           "org.bluez.Device1").Connect()

        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self._pump(0.1)
            if self._get(self.dev_path, "org.bluez.Device1",
                         "ServicesResolved"):
                break
        else:
            raise RuntimeError("ServicesResolved never became true")

        objects = self._managed()
        svc = self._find(objects, "org.bluez.GattService1", SVC_UUID)
        if not svc:
            raise RuntimeError("SomnoBridge service not found after connect")
        for path, ifaces in objects.items():
            c = ifaces.get("org.bluez.GattCharacteristic1")
            if not c or str(c.get("Service", "")) != svc:
                continue
            u = str(c.get("UUID", "")).lower()
            if u == CTRL_UUID:
                self.ctrl_path = path
            elif u == DATA_UUID:
                self.data_path = path
        if not self.ctrl_path:
            raise RuntimeError("control characteristic not found")

        self.bus.add_signal_receiver(
            self._on_value, signal_name="PropertiesChanged",
            dbus_interface="org.freedesktop.DBus.Properties")
        self._ctrl = dbus.Interface(self.bus.get_object("org.bluez",
                                                        self.ctrl_path),
                                    "org.bluez.GattCharacteristic1")
        if self.data_path:
            self._data = dbus.Interface(
                self.bus.get_object("org.bluez", self.data_path),
                "org.bluez.GattCharacteristic1")
        self.log("connected; ctrl", self.ctrl_path)

    def subscribe(self):
        self._ctrl.StartNotify()
        if self._data:
            self._data.StartNotify()
        self._pump(0.5)

    # ---------- inbound ----------

    def _on_value(self, interface, changed, invalidated):
        if "Value" not in changed:
            return
        data = bytes(bytearray(changed["Value"]))
        if len(data) < 2:
            return
        # compact stream-data frame: [ver][kind=10][seq:u16][payload]
        if data[0] == 1 and data[1] == K_STREAM_DATA:
            if not self._stream_open:
                return
            seq = data[2] | (data[3] << 8)
            self._stream_frames += 1
            if seq != (self._stream_seq & 0xFFFF):
                dist = (seq - (self._stream_seq & 0xFFFF)) & 0xFFFF
                if dist >= 0x8000:
                    # duplicate delivery (e.g. a second BlueZ notify
                    # session re-emits every frame): drop, don't append
                    self._dup_frames += 1
                    return
                # missed frame(s): count the gap, then resync so later
                # frames don't each count as an error
                self._seq_errors += dist
                self._stream_seq = seq
            self._stream_seq += 1
            self._stream += data[CHDR_LEN:]
            self._stream_bytes = len(self._stream)
            self._ack_pending = True
            return
        if len(data) < HDR_LEN:
            return
        ver, kind, flags, rid, off = HDR.unpack(data[:HDR_LEN])
        if ver != 1:
            return
        payload = data[HDR_LEN:]

        if kind == K_JSON_RESP:
            if rid in self._done and not (off == 0 and flags & FLAG_START):
                # duplicate delivery of a tail fragment after the
                # message completed — ignore rather than reopen a
                # buffer and poison the finished response
                return
            buf = self._rx.setdefault(rid, bytearray())
            if off == 0 and flags & FLAG_START and rid in self._rx_poisoned:
                # device retried the whole message (STREAM_END resend):
                # clear the poison and reassemble fresh
                self._rx_poisoned.discard(rid)
                del buf[:]
            if off > len(buf):
                # a fragment was lost at the host (BlueZ can drop
                # indications under load) — poison rather than
                # zero-fill so the caller sees a clean failure
                buf.extend(b"\x00" * (off - len(buf)))
                self._rx_poisoned.add(rid)
            buf[off:off + len(payload)] = payload
            if flags & FLAG_END:
                if rid in self._rx_poisoned:
                    self._done[rid] = None
                    self._rx_poisoned.discard(rid)
                else:
                    self._done[rid] = \
                        bytes(buf).decode("utf-8", "replace")
                self._rx.pop(rid, None)
        elif kind == K_TIME_REPLY:
            # t4 = receipt of the *complete* reply (§8.1) — stamp UTC and
            # monotonic together here, not after the pump loop.
            self._time_reply[rid] = (payload, time.time_ns() // 1000,
                                     time.monotonic_ns() // 1000)
        elif kind in (K_FILE_BYTES, K_EV_BYTES):
            if not self._stream_open or rid != self._stream_rid:
                return
            if off > len(self._stream):
                self._stream.extend(b"\x00" * (off - len(self._stream)))
            self._stream[off:off + len(payload)] = payload
            self._stream_bytes = max(self._stream_bytes, off + len(payload))
            # Do NOT write from inside the signal handler: a blocking
            # D-Bus call here stalls dispatch and loses signals. Flag it
            # and let the stream loop send the credit update instead.
            self._ack_pending = True

    # ---------- outbound ----------

    def _write(self, path, obj, blob, busy_ms=4000, cmd=False):
        value = dbus.Array([dbus.Byte(b) for b in blob], signature="y")
        # "command" = write-without-response: no ATT response to wait
        # for, so it can never stall behind a saturated notify path.
        opts = dbus.Dictionary({"offset": dbus.UInt16(0),
                                "type": "command" if cmd else "request"},
                               signature="sv")
        last = None
        busy_until = time.monotonic() + busy_ms / 1000.0
        for _ in range(8):
            try:
                obj.WriteValue(value, opts)
                return
            except dbus.DBusException as e:
                last = e
                name = getattr(e, "get_dbus_name", lambda: "")() or ""
                # one outstanding GATT request per connection: a slow
                # device-side response leaves the next write InProgress.
                # It clears as soon as the pending op completes.
                if "InProgress" in name or "Busy" in name:
                    if time.monotonic() < busy_until:
                        time.sleep(0.05)
                        continue
                    break
                # other transient GATT errors (auth flap): pace + retry
                time.sleep(0.05)
        raise last

    def _ack(self, rid, offset, window_bytes=None):
        mtu = int(self._get(self.ctrl_path, "org.bluez.GattCharacteristic1",
                            "MTU", 247) or 247)
        pay = max(mtu - 3 - HDR_LEN, 20)
        window = window_bytes if window_bytes is not None else 128 * pay
        blob = frame(K_ACK, rid, offset, 0, struct.pack("<I", window))
        # write-without-response: credit grants are idempotent (the
        # device tracks max right-edge) so a fire-and-forget write is
        # safe, and it avoids queueing behind in-flight GATT ops while
        # the notify path is saturated
        self._write(self.ctrl_path, self._ctrl, blob, cmd=True)

    def _next_rid(self):
        self._rid = (self._rid % 0xFFFFFF) + 1
        return self._rid

    def request(self, op, args=None, timeout=15.0):
        doc = {"op": op}
        if args:
            doc["args"] = args
        body = json.dumps(doc, separators=(",", ":")).encode()
        last = None
        for _ in range(2):                   # retry once on a lost fragment
            rid = self._next_rid()
            blob = frame(K_JSON_REQ, rid, 0, FLAG_START | FLAG_END, body)
            self._write(self.ctrl_path, self._ctrl, blob)
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                # fine-grained pump: the device serialises indications
                # and waits for our ATT confirmation — confirm promptly
                self._pump(0.005)
                if rid in self._done:
                    resp = self._done.pop(rid)
                    if resp is None:         # poisoned: fragment lost
                        last = TimeoutError(f"{op}: fragment lost")
                        break
                    return json.loads(resp)
            if last is None:
                last = TimeoutError(f"{op}: no response in {timeout:.0f}s")
        raise last

    # ---------- §8.1 time exchange ----------

    def time_exchange(self, unc_us=None, epoch=None, rounds=5, timeout=2.0,
                      gap=0.15, source=None, commit=True):
        """rounds x (TIME_REQUEST -> TIME_REPLY), then commit the best.

        The host time source is measured unless an explicit bounded override is
        supplied. A >100 ms source is accepted only as best-effort evidence;
        >500 ms prevents device-side anchor commitment entirely.

        TIME_REPLY is an ATT indication, so a new request sent before the
        previous one is confirmed gets no reply (NimBLE EBUSY) — hence the
        gap. The commit must land within 5 s of the chosen exchange (§8.1)
        so a missing reply must fail fast, not stall the whole round.
        """
        if source is None:
            source = clock_source(None if unc_us is None else unc_us / 1000,
                                  epoch)
        src_unc_us = int(math.ceil(source["uncertainty_ms"] * 1000))
        src_epoch = int(epoch if epoch is not None
                        else source.get("epoch", 1)) & 0xffffffff
        results = []
        for i in range(rounds):
            if i:
                self._pump(gap)
            rid = self._next_rid()
            t1 = time.time_ns() // 1000
            m1 = time.monotonic_ns() // 1000
            self._write(self.ctrl_path, self._ctrl,
                        frame(K_TIME_REQ, rid, 0, FLAG_START | FLAG_END,
                              struct.pack("<QII", t1, src_unc_us,
                                          src_epoch)))
            end = time.monotonic() + timeout
            while time.monotonic() < end and rid not in self._time_reply \
                    and rid not in self._done:
                self._pump(0.005)
            if rid in self._done:      # device answered with a JSON error
                results.append({"rid": rid, "ok": False,
                                "why": (self._done.pop(rid) or
                                        "fragment lost")[:120]})
                continue
            raw = self._time_reply.pop(rid, None)
            if raw is None:
                results.append({"rid": rid, "ok": False,
                                "why": "no reply"})
                continue
            raw, t4, m4 = raw
            if len(raw) < 28:      # §10.4: t1,m2,m3 (u64) + epoch (u32)
                results.append({"rid": rid, "ok": False,
                                "why": f"malformed TIME_REPLY ({len(raw)} B, "
                                       f"need 28)"})
                continue
            t1r, m2, m3, ep = struct.unpack("<QQQI", raw[:28])
            rtt = (t4 - t1r) - (m3 - m2)
            step_us = abs((t4 - t1r) - (m4 - m1))
            stepped = step_us > 10000
            results.append({
                "rid": rid, "ok": True, "t1": t1r, "t4": t4,
                "t1_mono_us": m1, "t4_mono_us": m4,
                "m2": m2, "m3": m3, "epoch": ep, "rtt_us": rtt,
                "proc_us": m3 - m2, "host_clock_step_us": step_us,
                "host_clock_step": stepped,
                "source_uncertainty_us": src_unc_us,
                "unc_us": src_unc_us + max(rtt, 0) // 2 +
                TIMESTAMP_ALLOWANCE_US,
                "usable": (0 <= rtt <= RTT_MAX_US and
                           src_unc_us <= SRC_UNC_MAX_US and not stepped),
                "qualified_source": src_unc_us <= SRC_QUAL_UNC_MAX_US,
            })

        response = {"source": source, "exchanges": results}
        usable = [r for r in results if r.get("usable")]
        if not commit:
            response.update({"committed": None,
                             "why": "commit_disabled"})
            return response
        if not usable:
            response.update({"committed": None,
                             "why": "no usable exchange (source<=500ms and "
                                    "rtt<=250ms required)"})
            return response

        best = min(usable, key=lambda r: r["rtt_us"])
        commit_frame = frame(K_TIME_COMMIT, best["rid"], 0,
                             FLAG_START | FLAG_END,
                             struct.pack("<QQI", best["t1"], best["t4"],
                                         best["epoch"]))
        self._write(self.ctrl_path, self._ctrl, commit_frame)
        end = time.monotonic() + timeout
        ack = None
        while time.monotonic() < end and best["rid"] not in self._done:
            self._pump(0.05)
        if best["rid"] in self._done:
            ack_raw = self._done.pop(best["rid"])
            ack = json.loads(ack_raw) if ack_raw else \
                {"ok": False, "error": "fragment_lost"}
        response.update({"committed": best, "ack": ack})
        return response

    # ---------- shared stream machinery (§10.2/§10.3) ----------

    def _stream_open_op(self, op, args=None, timeout=45.0):
        """Send a streaming op and wait for its STREAM_BEGIN response."""
        rid = self._next_rid()
        self._stream = bytearray()
        self._stream_bytes = 0
        self._stream_end = None
        self._stream_open = False
        self._stream_seq = 0
        self._seq_errors = 0
        self._last_acked = 0
        self._ack_pending = False
        self._stream_frames = 0
        self._dup_frames = 0
        self._stream_rid = rid
        doc = {"op": op}
        if args:
            doc["args"] = args
        self._write(self.ctrl_path, self._ctrl,
                    frame(K_JSON_REQ, rid, 0, FLAG_START | FLAG_END,
                          json.dumps(doc).encode()))
        # STREAM_BEGIN arrives as a JSON response; credit must follow or
        # the device's streamer waits forever (§10.3).
        begin = None
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self._pump(0.02)
            if rid in self._done:
                raw_begin = self._done.pop(rid)
                begin = json.loads(raw_begin) if raw_begin else \
                    {"ok": False, "error": {"code": "fragment_lost"}}
                break
        return rid, begin, end

    def _stream_collect(self, rid, end=None, idle=30.0):
        """Drive the credit/byte/end flow after a successful STREAM_BEGIN.
        The deadline is rolling: it expires only after `idle` seconds
        without stream progress, so arbitrarily large transfers work.
        `end` (absolute, from _stream_open_op) still bounds the initial
        credit wait."""
        self._stream_open = True
        last_progress = time.monotonic()
        last_bytes = -1
        try:
            self._ack(rid, 0, STREAM_WINDOW)
            while True:
                now = time.monotonic()
                if self._stream_bytes != last_bytes:
                    last_bytes = self._stream_bytes
                    last_progress = now
                elif now - last_progress > idle:
                    break                       # stalled: abort
                if end is not None and now > end and last_bytes <= 0:
                    break                       # never started
                self._pump(0.02)
                # right-edge credits: re-grant after ~¼ window of
                # contiguous wire bytes has arrived (update7 §10.3)
                if self._ack_pending and \
                        self._stream_bytes - self._last_acked >= \
                        STREAM_WINDOW // 4:
                    self._ack_pending = False
                    self._last_acked = self._stream_bytes
                    self._ack(rid, self._stream_bytes, STREAM_WINDOW)
                if rid in self._done:          # STREAM_END
                    raw_end = self._done.pop(rid)
                    if raw_end is None:
                        self._stream_end = {"ok": False,
                                            "error": "fragment_lost"}
                    else:
                        try:
                            self._stream_end = json.loads(raw_end)
                        except ValueError:
                            self._stream_end = {"ok": False,
                                                "error": "bad_end",
                                                "raw": raw_end[:200]}
                    break
        finally:
            self._stream_open = False
            if self._stream_end is None:
                try:
                    self._write(self.ctrl_path, self._ctrl,
                                frame(K_ABORT, rid, 0, FLAG_START | FLAG_END))
                except dbus.exceptions.DBusException as error:
                    self.log("stream abort failed:", error)
        return {"wire": bytes(self._stream), "end": self._stream_end,
                "seq_errors": self._seq_errors,
                "rx_frames": self._stream_frames,
                "dup_frames": self._dup_frames}

    # ---------- update7 block codec (§10.5) ----------

    @staticmethod
    def _inv_delta16(b):
        """LE u16 running-sum reconstruction of a delta16 block."""
        if _np is not None:
            a = _np.frombuffer(b, dtype="<u2").copy()
            _np.cumsum(a, dtype=_np.uint16, out=a)
            out = a.tobytes()
            if len(b) & 1:
                out += b[-1:]
            return out
        out = bytearray(len(b))
        acc = 0
        for i in range(0, len(b) & ~1, 2):
            acc = (acc + (b[i] | (b[i + 1] << 8))) & 0xFFFF
            out[i] = acc & 0xFF
            out[i + 1] = acc >> 8
        if len(b) & 1:
            out[-1] = b[-1]
        return bytes(out)

    def _decode_wire(self, wire):
        """Walk the block envelope; return (raw_bytes, stats)."""
        out = bytearray()
        pos = 0
        nblk = {"raw": 0, "def": 0, "d16": 0}
        while pos + BLK_HDR.size <= len(wire):
            codec, ver, _rsv, raw_len, enc_len = \
                BLK_HDR.unpack_from(wire, pos)
            if ver != 1:
                raise ValueError(f"bad block ver {ver} at +{pos}")
            enc = wire[pos + BLK_HDR.size:pos + BLK_HDR.size + enc_len]
            if len(enc) < enc_len:
                break               # truncated tail (aborted stream)
            if codec == 0:
                raw = enc
                nblk["raw"] += 1
            elif codec == 1:
                raw = zlib.decompress(enc, -15)
                nblk["def"] += 1
            elif codec == 2:
                raw = self._inv_delta16(zlib.decompress(enc, -15))
                nblk["d16"] += 1
            else:
                raise ValueError(f"bad codec {codec} at +{pos}")
            if len(raw) != raw_len:
                raise ValueError(
                    f"block raw_len {raw_len} != decoded {len(raw)}")
            out += raw
            pos += BLK_HDR.size + enc_len
        return bytes(out), nblk, pos

    def evidence(self, timeout=45.0):
        rid, begin, end = self._stream_open_op("GET_EVIDENCE",
                                             args={"v2": True},
                                             timeout=timeout)
        if not begin or not begin.get("ok"):
            return {"begin": begin, "raw": b"", "end": None}
        res = self._stream_collect(rid, end)
        if (begin.get("result") or {}).get("wire") == 2:
            self._decode_guarded(res)
        else:
            res["raw"], res["blocks"] = res["wire"], {"raw": 0}
            res["wire_consumed"] = len(res["wire"])
        res["begin"] = begin
        return res

    def get_file(self, directory, name, expected_size=None, timeout=120.0):
        args = {"directory": directory, "name": name, "v2": True}
        if expected_size is not None:
            args["expected_size"] = str(expected_size)
        for attempt in range(3):
            rid, begin, end = self._stream_open_op("GET_FILE", args,
                                                 timeout=timeout)
            if not begin or not begin.get("ok"):
                code = ((begin or {}).get("error") or {}).get("code")
                if code == "STALE" and attempt < 2:
                    # day listing older than 90 s: the device queued a
                    # rescan (~10 s) and answered honestly — wait it out
                    time.sleep(12)
                    continue
                return {"begin": begin, "raw": b"", "end": None}
            res = self._stream_collect(rid, end)
            if res.get("end") is not None:
                if (begin.get("result") or {}).get("wire") == 2:
                    self._decode_guarded(res)
                else:
                    res["raw"], res["blocks"] = \
                        res["wire"], {"raw": 0}
                    res["wire_consumed"] = len(res["wire"])
            res["begin"] = begin
            return res

    def _decode_guarded(self, res):
        """Decode wire-v2 blocks; a host-side lost notification
        corrupts the deflate stream — report instead of raising."""
        try:
            res["raw"], res["blocks"], res["wire_consumed"] = \
                self._decode_wire(res["wire"])
        except (ValueError, zlib.error) as e:
            res["raw"], res["blocks"], res["wire_consumed"] = \
                b"", None, 0
            res["decode_error"] = str(e)
        return res

    def stream_test(self, nbytes, pattern="ramp", v2=True,
                    timeout=120.0):
        """Synthetic bulk stream — codec+wire only, no card access."""
        rid, begin, end = self._stream_open_op(
            "STREAM_TEST",
            args={"bytes": nbytes, "pattern": pattern, "v2": v2},
            timeout=timeout)
        if not begin or not begin.get("ok"):
            return {"begin": begin, "raw": b"", "end": None}
        res = self._stream_collect(rid, end)
        if res.get("end") is not None:
            if (begin.get("result") or {}).get("wire") == 2:
                self._decode_guarded(res)
            else:
                res["raw"], res["blocks"] = res["wire"], {"raw": 0}
                res["wire_consumed"] = len(res["wire"])
        res["begin"] = begin
        return res

    def prepare_days(self, days):
        """Batch pre-refresh of DATALOG day listings (update7 §10.2)."""
        return self.request("PREPARE_DAYS", {"days": sorted(set(days))},
                            timeout=15.0)

    def close(self):
        try:
            if self._ctrl is not None:
                self._ctrl.StopNotify()
            if self._data is not None:
                self._data.StopNotify()
        except dbus.exceptions.DBusException:
            pass


# ---------- formatting ----------

def hr(title):
    print(f"\n=== {title} ===")


def fmt_ms(v):
    return "null" if v is None else f"{v} ms"


def fmt_iv(iv):
    return "null" if not iv else f"[{iv[0]}, {iv[1]}] ms"


def print_status(r):
    res = r.get("result", {})
    hr("GET_STATUS")
    print(f"  fw {res.get('fw')} proto {res.get('proto')}   "
          f"security={res.get('security')} mode={res.get('mode')}")
    print(f"  sta_connected={res.get('sta_connected')} rssi={res.get('rssi')} "
          f"paired={res.get('paired')} ble_connected={res.get('ble_connected')}")
    t = res.get("therapy", {})
    print(f"  therapy: state={t.get('state')} stale={t.get('stale')} "
          f"reason={t.get('reason')} confidence={t.get('confidence')}")
    print(f"           expected_append={t.get('expected_append_ms')} ms  "
          f"out_timeout={t.get('out_timeout_ms')} ms  "
          f"transfer_allowed={t.get('transfer_allowed')}")
    b = res.get("budget", {})
    cap = f" cap={b.get('cap')}" if "cap" in b else ""
    print(f"  requests: active_8h={b.get('active_8h')} "
          f"backoff={b.get('backoff_ms')} ms{cap} "
          f"(diagnostic — no cumulative cap since update6)")
    ph = res.get("phase")
    if ph:
        print(f"  phase: st={ph.get('st')} day={ph.get('day')} "
              f"armed={ph.get('armed')} pulls={ph.get('pulls')} "
              f"strategy={ph.get('strategy')}")
        print(f"         bracket={ph.get('bracket_ms')} ms "
              f"prediction_iunc={ph.get('prediction_iunc_ms',
                                              ph.get('iunc_ms'))} ms "
              f"period={ph.get('period_ms')} ms "
              f"mark_in={ph.get('mark_in_ms')} ms "
              f"brackets={ph.get('brackets')} misses={ph.get('misses')}")
    http = res.get("http")
    if http:
        last = http.get("last") or {}
        svc = http.get("service_us") or {}
        print(f"  http: last={last.get('purpose')} ok={last.get('ok')} "
              f"service={last.get('service_us')} us "
              f"req={last.get('req_start_us')} "
              f"resp={last.get('resp_end_us')}")
        print(f"       service_us n={svc.get('n')} "
              f"min={svc.get('min')} median={svc.get('median')} "
              f"max={svc.get('max')} failures={svc.get('failures')}")


def print_catalog(r):
    res = r.get("result", {})
    hr("GET_CATALOG_STATUS")
    print(f"  state={res.get('state')} folders={res.get('folders_total')} "
          f"catalog_complete={res.get('catalog_complete')} "
          f"hot_ready={res.get('hot_ready')}")
    print(f"  incomplete={res.get('incomplete_count')} "
          f"psram={res.get('psram_used')} hot_days={res.get('hot_days')}")


def print_days(r):
    res = r.get("result", {})
    days = res.get("days", [])
    hr("LIST_RECENT_DAYS")
    print(f"  {len(days)} day(s), newest first: "
          f"{', '.join(days[:12])}{' ...' if len(days) > 12 else ''}")
    print(f"  catalog_complete={res.get('catalog_complete')} "
          f"hot_ready={res.get('hot_ready')}")


def print_day(r, day):
    hr(f"LIST_DAY {day}")
    if not r.get("ok"):
        print(f"  unavailable: {r.get('error')}")
        return
    res = r.get("result", {})
    files = res.get("files", [])
    print(f"  {len(files)} file(s), scan_age_us={res.get('scan_age_us')}")
    for f in files[:20]:
        print(f"    {f.get('name'):<32} {f.get('size'):>10} B  ts={f.get('ts')}")
    if len(files) > 20:
        print(f"    ... {len(files) - 20} more")


def print_ls(r, directory):
    hr(f"LIST_DIRECTORY {directory}")
    if not r.get("ok"):
        print(f"  card error: {r.get('error')}")
        return
    res = r.get("result", {})
    files = res.get("files", [])
    print(f"  {len(files)} entr(ies)")
    for f in files[:25]:
        print(f"    {f.get('name'):<32} {f.get('size'):>10} B  ts={f.get('ts')}")


def print_drift(r):
    """U10 envelope: {"schema":5,"estimate":{...}|null,
                     "source_of_truth":{...}|null,
                     "completed_session_fallback":{...}|null}."""
    res = r.get("result", {})
    hr("GET_DRIFT")
    if not r.get("ok"):
        print(f"  unavailable: {r.get('error')}")
        return
    est = res.get("estimate")
    print("  estimate:")
    if not est:
        print("    (none — no session observed this boot)")
    else:
        files = est.get("files", [])
        print(f"    estimate_state={est.get('estimate_state')} "
              f"tier={est.get('accuracy_tier')} "
              f"closed={est.get('observation_closed')}")
        print(f"    source={est.get('preferred_source')} "
              f"({est.get('source_reason')})")
        fdir = files[0].get('dir', '') if files else ''
        day = fdir.rsplit('\\', 1)[-1] if fdir else '-'
        print(f"    beacon: {day}/"
              f"{files[0].get('name') if files else '-'}")
        print(f"    clock_drift_ms    = {fmt_ms(est.get('clock_drift_ms'))}"
              f"   interval {fmt_iv(est.get('drift_interval_ms'))}")
        print(f"    accuracy_half_ms  = "
              f"{fmt_ms(est.get('accuracy_half_ms'))}")
        print(f"    computed_at_ms    = {fmt_ms(est.get('computed_at_ms'))}  "
              f"reference_at_ms = {fmt_ms(est.get('reference_at_ms'))}")
        print(f"    session_start_as10={est.get('session_start_as10')}  "
              f"true={fmt_ms(est.get('session_start_true_ms'))} "
              f"interval {fmt_iv(est.get('start_interval_ms'))}")
        print(f"    flush_period_ms   = {fmt_ms(est.get('flush_period_ms'))}"
              f"   interval {fmt_iv(est.get('flush_period_interval_ms'))}")
        print(f"    candidates: mtime={fmt_iv(est.get('mtime_interval_ms'))} "
              f"create={fmt_iv(est.get('filename_creation_interval_ms'))} "
              f"periodic={fmt_iv(est.get('filename_periodic_interval_ms'))}")
        print(f"    unavailable: create="
              f"{est.get('filename_creation_reason')} periodic="
              f"{est.get('filename_periodic_reason')}")
        tm = est.get("timing_model") or {}
        print(f"    timing_model: rev={tm.get('model_rev')} "
              f"cache={tm.get('cache_policy')} "
              f"cache_us={tm.get('cache_bound_us')} "
              f"visibility={tm.get('visibility_policy')} "
              f"delay_us={tm.get('visibility_delay_us')} "
              f"jitter_us={tm.get('jitter_us')} "
              f"dev_rate_ppm={tm.get('dev_rate_ppm')}")
        obs = est.get("recent_observations") or []
        print(f"    recent_observations={len(obs)}")
        for ob in obs:
            print(f"      obs {ob.get('seq')} {ob.get('kind')} "
                  f"bracket={ob.get('bracket_us')} us "
                  f"svc={ob.get('unchanged_service_us')}/"
                  f"{ob.get('changed_service_us')} us "
                  f"utc={ob.get('visibility_utc_ms')} "
                  f"off={ob.get('offset_interval_ms')}")
        print(f"    windows={est.get('n_windows')} "
              f"segments={est.get('segments')}")
        flags = est.get("flags") or []
        print(f"    flags: {', '.join(flags) if flags else '(none)'}")
    schema = res.get("schema")

    def print_sel(v, label):
        """schema>=5 selected-view object (drift_sel_t)."""
        if not v:
            print("    (none)")
            return
        print(f"    selected_view     = {v.get('selected_view')}"
              f"   reason={v.get('selection_reason')}")
        print(f"    quality={v.get('quality')} "
              f"source={v.get('source')} "
              f"interval={fmt_iv(v.get('interval_ms'))} "
              f"half={fmt_ms(v.get('accuracy_half_ms'))}")
        print(f"    measured          = "
              f"{fmt_iv(v.get('measurement_interval_ms'))} "
              f"half={fmt_ms(v.get('measurement_half_ms'))} "
              f"age={fmt_ms(v.get('measurement_reference_age_ms'))} "
              f"allowance={fmt_ms(v.get('fallback_adjustment_ms'))}")
        print(f"    model_rev={v.get('model_rev')} "
              f"model_id={v.get('model_id')} "
              f"assumptions={v.get('assumptions')}")

    def sot_rec(v):
        """Unwrap schema>=5 historical slot wrapper."""
        if isinstance(v, dict) and "record" in v:
            return v.get("record"), v.get("historical")
        return v, None

    sot = res.get("source_of_truth")
    print("  source_of_truth:")
    if not sot:
        print("    (none — no live qualified correction)")
    elif schema and schema >= 5:
        print_sel(sot, "source_of_truth")
    else:
        print(f"    clock_drift_ms    = {fmt_ms(sot.get('clock_drift_ms'))}"
              f"   interval {fmt_iv(sot.get('interval_ms'))}")
        print(f"    accuracy_half_ms  = {fmt_ms(sot.get('accuracy_half_ms'))} "
              f"origin={sot.get('origin')} seq={sot.get('persisted_seq')}")
        print(f"    computed_at_ms    = {fmt_ms(sot.get('computed_at_ms'))}  "
              f"reference_at_ms = {fmt_ms(sot.get('reference_at_ms'))}")
        print(f"    quality={sot.get('quality')} source={sot.get('source')} "
              f"stale={sot.get('stale')} applicable={sot.get('applicable')} "
              f"freshness_unknown={sot.get('freshness_unknown')}")
        print(f"    model_rev={sot.get('model_rev')} "
              f"cache={sot.get('cache_policy')} "
              f"visibility={sot.get('visibility_policy')} "
              f"delay_us={sot.get('visibility_delay_us')}")
    for key in ("qualified_source_of_truth",
                    "provisional_source_of_truth"):
        slot = res.get(key)
        if slot is None:
            continue
        rec, hist = sot_rec(slot)
        label = key + (" (historical)" if hist else "")
        print(f"  {label}:")
        if not rec:
            print("    (none)")
        else:
            print(f"    interval={fmt_iv(rec.get('interval_ms'))} "
                  f"half={fmt_ms(rec.get('accuracy_half_ms'))} "
                  f"source={rec.get('source')} stale={rec.get('stale')} "
                  f"model_rev={rec.get('model_rev')}")
    fb = res.get("completed_session_fallback")
    if fb is not None:
        print("  completed_session_fallback:")
        if not fb:
            print("    (none persisted)")
        else:
            rec = fb.get("record") or {}
            print(f"    fallback_eligible={fb.get('fallback_eligible')} "
                  f"age={fmt_ms(fb.get('age_ms'))} "
                  f"adjusted_half={fmt_ms(fb.get('adjusted_half_ms'))} "
                  f"(+{fmt_ms(fb.get('age_allowance_ms'))} allowance, "
                  f"max {fmt_ms(fb.get('max_age_ms'))})")
            print(f"    interval={fmt_iv(rec.get('interval_ms'))} "
                  f"half={fmt_ms(rec.get('accuracy_half_ms'))} "
                  f"quality={rec.get('quality')} "
                  f"stale={rec.get('stale')} "
                  f"freshness_unknown={rec.get('freshness_unknown')} "
                  f"model_rev={rec.get('model_rev')}")
            print(f"    reference_at_ms={fmt_ms(rec.get('reference_at_ms'))}"
                  f"  seq={rec.get('persisted_seq')}")
    best = res.get("best_available_offset")
    if best is not None:
        print("  best_available_offset:")
        if not best:
            print(f"    (none — {res.get('best_available_reason')})")
        elif schema and schema >= 5:
            print_sel(best, "best_available_offset")
        else:
            print(f"    quality={best.get('quality')} "
                  f"source={best.get('source')} "
                  f"interval={fmt_iv(best.get('interval_ms'))} "
                  f"half={fmt_ms(best.get('accuracy_half_ms'))}")
            print(f"    assumptions={best.get('assumptions')} "
                  f"freshness_unknown={best.get('freshness_unknown')}")


def print_time_sync(r):
    hr("TIME SYNC (§8.1)")
    src = r.get("source") or {}
    if src:
        print(f"  source={src.get('source')} "
              f"uncertainty=±{src.get('uncertainty_ms')} ms "
              f"epoch={src.get('epoch')}")
    for i, ex in enumerate(r.get("exchanges", []), 1):
        if not ex.get("ok"):
            print(f"  #{i} rid={ex['rid']} FAILED ({ex.get('why')})")
            continue
        print(f"  #{i} rid={ex['rid']} rtt={ex['rtt_us'] / 1000:.2f} ms  "
              f"on-device={ex['proc_us'] / 1000:.2f} ms  "
              f"unc=±{ex['unc_us'] / 1000:.2f} ms  "
              f"{('usable/qualified' if ex.get('qualified_source') else
                 'usable/best-effort') if ex['usable'] else 'REJECTED'}"
              f"{' host_step' if ex.get('host_clock_step') else ''}")
    best = r.get("committed")
    if not best:
        print(f"  committed: none — {r.get('why')}")
        return
    print(f"  committed rid={best['rid']} "
          f"(rtt={best['rtt_us'] / 1000:.2f} ms, unc=±{best['unc_us'] / 1000:.2f} ms)")
    ack = r.get("ack")
    if not ack:
        print("  device ack: MISSING")
        return
    a = ack.get("result", {})
    print(f"  device ack: accepted={a.get('accepted')} reason={a.get('reason')} "
          f"uncertainty_us={a.get('uncertainty_us')} anchors={a.get('anchors')}")


def print_evidence(r):
    hr("GET_EVIDENCE")
    begin = r.get("begin") or {}
    res = begin.get("result", {})
    print(f"  begin: journal_seq={res.get('journal_seq')} "
          f"oldest_seq={res.get('oldest_seq')} truncated={res.get('truncated')}")
    raw = r.get("raw", b"")
    print(f"  {len(raw)} payload byte(s)")
    text = raw.decode("utf-8", "replace")
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            print(f"    {line[:120]}")
            continue
        kind = rec.get("k") or rec.get("type") or "?"
        rest = {k: v for k, v in rec.items() if k not in ("k", "type")}
        print(f"    [{kind}] {json.dumps(rest, separators=(',', ':'))[:140]}")
    end = r.get("end") or {}
    e = end.get("result", {})
    print(f"  end: ok={e.get('ok')} bytes={e.get('actual_bytes')} "
          f"gaps={e.get('gaps')} sha256={str(e.get('sha256'))[:16]}...")


# ---------- main ----------

def self_test():
    from unittest.mock import Mock, patch

    def cframe(seq, payload):
        return bytes([1, K_STREAM_DATA, seq & 0xFF, seq >> 8]) + payload

    def blk(raw):
        return BLK_HDR.pack(0, 1, 0, len(raw), len(raw)) + raw

    def client():
        dev = SomnoBridge.__new__(SomnoBridge)
        dev._next_rid = Mock(return_value=42)
        dev._write = Mock()
        dev._ack = Mock()
        dev.log = Mock()
        dev.ctrl_path = dev._ctrl = None
        dev._done = {}
        dev._rx = {}
        return dev

    begin = {"ok": True, "result": {"truncated": False, "wire": 2}}
    end = {"ok": True, "result": {"ok": True}}
    dev = client()
    wire = blk(b"current")
    def pump(seconds):
        if not dev._stream_open:
            dev._done[42] = json.dumps(begin)
        else:
            # a stale-rid legacy frame must be ignored
            dev._on_value(None, {"Value": frame(K_FILE_BYTES, 41, 0, 0,
                                                b"stale")}, [])
            dev._on_value(None, {"Value": cframe(0, wire[:9])}, [])
            dev._on_value(None, {"Value": cframe(1, wire[9:])}, [])
            dev._done[42] = json.dumps(end)
    dev._pump = pump
    with patch(__name__ + ".time.monotonic", return_value=0):
        replay = dev.evidence()
    assert replay["raw"] == b"current" and replay["end"] == end
    assert dev._write.call_count == 1 and not dev._stream_open
    dev._on_value(None, {"Value": frame(K_FILE_BYTES, 42, 7, 0,
                                        b"late")}, [])
    dev._on_value(None, {"Value": cframe(9, b"late")}, [])
    assert bytes(dev._stream) == wire

    dev = client()
    dev._pump = lambda seconds: dev._done.update({42: json.dumps(begin)})
    with patch(__name__ + ".time.monotonic", side_effect=[0, 0, 2, 2]):
        replay = dev.evidence(timeout=1)
    assert replay["end"] is None and not dev._stream_open
    assert HDR.unpack(dev._write.call_args.args[2][:HDR_LEN])[1] == K_ABORT

    dev = client()
    dev._pump = lambda seconds: dev._done.update({42: json.dumps(begin)})
    dev._ack.side_effect = RuntimeError("synthetic credit failure")
    with patch(__name__ + ".time.monotonic", return_value=0):
        try:
            dev.evidence()
        except RuntimeError:
            pass
        else:
            raise AssertionError("credit failure must propagate")
    assert not dev._stream_open
    assert HDR.unpack(dev._write.call_args.args[2][:HDR_LEN])[1] == K_ABORT

    # block codec: raw + delta16 + deflate round-trip
    dev = client()
    e = dev._decode_wire(blk(b"hello"))
    assert e[0] == b"hello" and e[2] == len(blk(b"hello"))
    # deflate-only block
    co = zlib.compressobj(6, zlib.DEFLATED, -15)
    enc = co.compress(b"aaaabbbb" * 100) + co.flush()
    e = dev._decode_wire(BLK_HDR.pack(1, 1, 0, 800, len(enc)) + enc)
    assert e[0] == b"aaaabbbb" * 100
    # delta16+deflate block
    raw = b"\x01\x00\x05\x00\x03\x00\x02\x00"
    co = zlib.compressobj(6, zlib.DEFLATED, -15)
    enc = co.compress(b"\x01\x00\x04\x00\xfe\xff\xff\xff") + co.flush()
    e = dev._decode_wire(BLK_HDR.pack(2, 1, 0, 8, len(enc)) + enc)
    assert e[0] == raw
    print("BLE stream self-tests passed")


def main():
    ap = argparse.ArgumentParser(
        description="Query a bonded SomnoBridge over BLE (BlueZ D-Bus).",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--address", default=DEFAULT_ADDR,
                    help=f"BLE address (default {DEFAULT_ADDR})")
    ap.add_argument("--status", action="store_true", help="GET_STATUS")
    ap.add_argument("--catalog", action="store_true", help="GET_CATALOG_STATUS")
    ap.add_argument("--days", type=int, nargs="?", const=14, default=None,
                    metavar="N", help="LIST_RECENT_DAYS (default 14)")
    ap.add_argument("--day", metavar="YYYYMMDD", help="LIST_DAY for a day")
    ap.add_argument("--ls", metavar="DIR",
                    help="LIST_DIRECTORY — live card listing, e.g. 'A:\\'")
    ap.add_argument("--drift", action="store_true",
                    help="GET_DRIFT — session estimate + qualified SOT")
    ap.add_argument("--drift-best", action="store_true",
                    help="also request the explicit best-available SOT view")
    ap.add_argument("--time-sync", type=int, nargs="?", const=5, default=None,
                    metavar="N",
                    help="run N exchanges and commit the best anchor (§8.1)")
    ap.add_argument("--source-uncertainty-ms", type=float,
                    help="explicit host UTC source bound (default: measured chrony bound)")
    ap.add_argument("--source-epoch", type=lambda x: int(x, 0),
                    help="explicit time-source epoch for §8.1 anchors")
    ap.add_argument("--no-time-commit", action="store_true",
                    help="collect TIME exchanges without committing an anchor")
    ap.add_argument("--get", metavar="PATH", nargs="+",
                    help="GET_FILE: download card file(s) (e.g. "
                         "'DATALOG/20261005/x.edf'); writes --out when a "
                         "single path is given")
    ap.add_argument("--out", metavar="FILE",
                    help="output path for --get (default: basename in cwd)")
    ap.add_argument("--evidence", action="store_true",
                    help="replay the evidence journal (GET_EVIDENCE)")
    ap.add_argument("--stream-test", type=int, metavar="BYTES",
                    help="STREAM_TEST: synthetic bulk stream (no card)")
    ap.add_argument("--pattern", choices=["ramp", "rand"],
                    default="ramp", help="STREAM_TEST data pattern")
    ap.add_argument("--v1", action="store_true",
                    help="use legacy wire framing (no compact/codec)")
    ap.add_argument("--json", action="store_true", help="print raw JSON")
    ap.add_argument("--timeout", type=float, default=15.0)
    ap.add_argument("--self-test", action="store_true",
                    help="run synthetic stream tests without connecting")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        self_test()
        return
    if args.drift_best:
        args.drift = True
    if not any([args.status, args.catalog, args.days is not None, args.day,
                args.drift, args.time_sync is not None,
                args.evidence, args.stream_test is not None]):
        args.status = args.catalog = args.drift = True
        args.days = 14

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    dev = SomnoBridge(args.address, verbose=args.verbose)
    try:
        dev.connect()
        dev.subscribe()

        if args.status:
            r = dev.request("GET_STATUS", timeout=args.timeout)
            print(json.dumps(r, indent=2)) if args.json else print_status(r)

        if args.catalog:
            r = dev.request("GET_CATALOG_STATUS", timeout=args.timeout)
            print(json.dumps(r, indent=2)) if args.json else print_catalog(r)

        days = []
        if args.days is not None:
            r = dev.request("LIST_RECENT_DAYS", {"days": args.days},
                            timeout=args.timeout)
            print(json.dumps(r, indent=2)) if args.json else print_days(r)
            days = r.get("result", {}).get("days", [])

        if args.day:
            r = dev.request("LIST_DAY", {"day": args.day},
                            timeout=args.timeout)
            print(json.dumps(r, indent=2)) if args.json else print_day(r, args.day)

        if args.ls:
            r = dev.request("LIST_DIRECTORY", {"directory": args.ls},
                            timeout=args.timeout)
            print(json.dumps(r, indent=2)) if args.json else print_ls(r, args.ls)

        if args.drift:
            req = {"best_available": True} if args.drift_best else {}
            r = dev.request("GET_DRIFT", req, timeout=args.timeout)
            print(json.dumps(r, indent=2)) if args.json else print_drift(r)

        if args.time_sync is not None:
            source = clock_source(args.source_uncertainty_ms,
                                  args.source_epoch)
            r = dev.time_exchange(rounds=args.time_sync, source=source,
                                  commit=not args.no_time_commit)
            print(json.dumps(r, indent=2)) if args.json else print_time_sync(r)

        if args.evidence:
            r = dev.evidence(timeout=args.timeout * 3)
            print(json.dumps(r, indent=2, default=str)) if args.json \
                else print_evidence(r)

        if args.stream_test is not None:
            t0 = time.monotonic()
            r = dev.stream_test(args.stream_test, pattern=args.pattern,
                                v2=not args.v1, timeout=args.timeout * 4)
            dt = time.monotonic() - t0
            if args.json:
                print(json.dumps(r, indent=2, default=str))
            else:
                raw = len(r.get("raw", b""))
                wire = len(r.get("wire", b""))
                endr = (r.get("end") or {}).get("result", {})
                print(f"STREAM_TEST {args.pattern} wire{'' if args.v1 else '2'}: "
                      f"raw={raw}B wire={wire}B in {dt:.1f}s -> "
                      f"{raw/dt/1024:.1f} KiB/s raw, "
                      f"{wire/dt/1024:.1f} KiB/s wire")
                print(f"  blocks={r.get('blocks')} "
                      f"end_ok={endr.get('ok')} sha256={endr.get('sha256','')[:12]} "
                      f"seq_errors={r.get('seq_errors')} "
                      f"decode_error={r.get('decode_error')} "
                      f"stall_ms={endr.get('stall_ms')} "
                      f"retry_ms={endr.get('retry_ms')} "
                      f"encode_ms={endr.get('encode_ms')}")

        if args.get:
            paths = args.get if isinstance(args.get, list) else [args.get]
            files = []
            for p in paths:
                parts = [c for c in re.split(r"[\\/]", p) if c]
                if len(parts) < 2:
                    sys.exit("--get needs DIR/NAME "
                             "(e.g. /DATALOG/20261005/x.edf)")
                files.append(("\\".join(parts[:-1]), parts[-1]))
            # batch pre-refresh: queue rescans for every stale DATALOG
            # day in one shot instead of a 12 s STALE retry each
            days = [d.split("\\")[-1] for d, _ in files
                    if "DATALOG" in d.upper()]
            if len(paths) > 1 and days:
                pr = dev.prepare_days(days)
                pr_res = (pr or {}).get("result") or {}
                eta = pr_res.get("eta_ms", 0)
                if eta:
                    print(f"  PREPARE_DAYS: queued rescans, "
                          f"waiting {eta} ms", file=sys.stderr)
                    time.sleep(eta / 1000 + 1)
            t_all = time.monotonic()
            tot_raw = tot_wire = 0
            n_ok = 0
            for i, (directory, name) in enumerate(files):
                t0 = time.monotonic()
                r = dev.get_file(directory, name,
                                 timeout=max(args.timeout * 6, 120))
                elapsed = time.monotonic() - t0
                end = (r.get("end") or {}).get("result") or {}
                raw = r.get("raw", b"")
                wire = r.get("wire", b"")
                sha = hashlib.sha256(raw).hexdigest()
                sha_ok = sha == end.get("sha256")
                bytes_ok = str(len(raw)) == end.get("actual_bytes")
                enc_ok = str(len(wire)) == end.get("encoded_bytes")
                ok = bool(end.get("ok")) and sha_ok and bytes_ok
                tot_raw += len(raw)
                tot_wire += len(wire)
                n_ok += ok
                if len(paths) == 1 or not ok:
                    hr(f"GET_FILE {name}")
                    print(f"  end:   {json.dumps(end)[:300]}")
                print(f"  [{i+1}/{len(files)}] {name}: {len(raw)} B in "
                      f"{elapsed:.1f}s "
                      f"({len(raw)/max(elapsed,0.01)/1024:.1f} KiB/s raw, "
                      f"{len(wire)/max(elapsed,0.01)/1024:.1f} KiB/s wire)  "
                      f"sha256={'ok' if sha_ok else 'MISMATCH'} "
                      f"bytes={'ok' if bytes_ok else 'MISMATCH'} "
                      f"wire_len={'ok' if enc_ok else str(len(wire))} "
                      f"seq_err={r.get('seq_errors', 0)} "
                      f"blk={r.get('blocks')}")
                if not ok:
                    print("  NOT saved (transfer incomplete or invalid)")
                elif not args.json:
                    out = args.out if (args.out and len(files) == 1) \
                        else name
                    with open(out, "wb") as f:
                        f.write(raw)
            if len(files) > 1:
                wall = time.monotonic() - t_all
                hr("SUMMARY")
                print(f"  {n_ok}/{len(files)} ok, {tot_raw} raw B in "
                      f"{wall:.1f}s "
                      f"({tot_raw/max(wall,0.01)/1024:.1f} KiB/s raw, "
                      f"{tot_wire/max(wall,0.01)/1024:.1f} KiB/s wire)")
    finally:
        dev.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
