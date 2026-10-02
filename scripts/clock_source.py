#!/usr/bin/env python3
# Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
# GPLv3+ — see LICENSE and NOTICE.
#
# Host clock-source bound and stable source identity for SomnoPal timing
# exchanges. The uncertainty is deliberately conservative: chrony's root
# distance plus a fixed guard is a bound claim, not a precision claim.

import math
import os
import re
import subprocess
import zlib


def _number(report, label):
    m = re.search(r"^" + re.escape(label) + r"\s*:\s*([-\d.]+)",
                  report, re.M)
    if not m:
        raise ValueError("missing chrony field: " + label)
    return abs(float(m.group(1)))


def _field(report, label):
    m = re.search(r"^" + re.escape(label) + r"\s*:\s*(.+)$",
                  report, re.M)
    return m.group(1).strip() if m else "unknown"


def clock_bound(override=None):
    if override is not None:
        if not math.isfinite(override) or override <= 0:
            raise ValueError("source uncertainty must be finite and positive")
        return {"source": "explicit", "uncertainty_ms": override,
                "provenance": "operator_override_not_independently_verified"}
    report = subprocess.check_output(["chronyc", "tracking"], text=True,
                                     timeout=5)
    if not re.search(r"Leap status\s*:\s*Normal", report):
        raise ValueError("chrony is not synchronized")
    bound = (_number(report, "Root delay") / 2 +
             _number(report, "Root dispersion") +
             _number(report, "System time")) * 1000
    return {"source": "chrony_root_distance",
            "uncertainty_ms": math.ceil(bound + 5),
            "rms_offset_ms": _number(report, "RMS offset") * 1000,
            "reference_id": _field(report, "Reference ID"),
            "stratum": _field(report, "Stratum"),
            "provenance": "chronyc_tracking_at_collection"}


def _boot_identity():
    try:
        with open("/proc/sys/kernel/random/boot_id", "r",
                  encoding="ascii") as f:
            return f.read().strip()
    except OSError:
        return os.uname().nodename


def clock_source(uncertainty_override=None, epoch_override=None):
    bound = clock_bound(uncertainty_override)
    if epoch_override is not None:
        epoch = int(epoch_override) & 0xffffffff
        basis = "explicit"
    elif bound["source"] == "chrony_root_distance":
        # Stable across routine NTP samples, but changes with the selected
        # reference or host boot. Clock steps are still caught by anchor
        # consistency checks and per-exchange wall/monotonic comparison.
        material = "|".join([bound.get("reference_id", "unknown"),
                             bound.get("stratum", "unknown"),
                             _boot_identity()])
        epoch = zlib.crc32(material.encode("ascii")) & 0xffffffff
        basis = "chrony_reference_plus_boot"
    else:
        epoch = zlib.crc32(("explicit-source|" + _boot_identity()).encode()) \
            & 0xffffffff
        basis = "explicit_plus_boot"
    return {**bound, "epoch": epoch, "epoch_basis": basis}
