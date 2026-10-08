#!/usr/bin/env python3
# SomnoBridge — host-side BLE pairing tool (LE Secure Connections, MITM).
# Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
# GPLv3+ — see LICENSE and NOTICE.
#
# Pairs this host with a SomnoBridge in one uninterrupted run:
#
#   1. Short-press the device button — the LCD shows "PAIRING 120S".
#   2. Run this script. It finds the device, connects, and starts pairing.
#   3. The device shows a 6-digit PIN on its LCD.
#   4. Type the PIN at the prompt in this terminal (you have ~25 s — the
#      SMP procedure times out at 30 s). For scripted use, pass
#      --code-file PATH and write the PIN into that file instead.
#
# The device is DisplayOnly (§3.1): it shows the PIN, the host enters it —
# so the BlueZ agent capability is KeyboardDisplay and pairing completes
# through RequestPasskey. Pair() is issued asynchronously and the GLib
# main loop keeps running so the agent's answers actually dispatch — a
# synchronous Pair() blocks the loop, the agent never answers, and the
# device cancels at its 30 s SMP timeout.
#
# Requires python3-dbus + python3-gi (same as ble_query.py).

import argparse
import os
import sys
import time

import dbus
import dbus.service
import dbus.mainloop.glib
from gi.repository import GLib

from ble_query import find_device_address, ADV_NAME_PREFIX

AGENT_PATH = "/somno/pair/agent"
DEFAULT_CODE_FILE = "/tmp/somno-pair-code.txt"
PASSKEY_POLL_S = 25          # < 30 s SMP timeout
PAIR_TIMEOUT_S = 90


def fail(msg, code=1):
    print(f"pair: {msg}", file=sys.stderr)
    sys.exit(code)


class Agent(dbus.service.Object):
    """BlueZ agent that answers from the terminal (or a code file)."""

    def __init__(self, bus, path, code_file, wait_s):
        self.code_file = code_file      # None -> interactive prompt
        self.wait_s = wait_s
        super().__init__(bus, path)

    def _from_file(self):
        """Poll the code file until it holds exactly six digits."""
        end = time.monotonic() + self.wait_s
        while time.monotonic() < end:
            try:
                with open(self.code_file) as f:
                    code = f.read().strip()
                if len(code) == 6 and code.isdigit():
                    return int(code)
                if code:
                    print(f"pair: {self.code_file}: want 6 digits, got "
                          f"{code!r} \u2014 waiting\u2026", file=sys.stderr)
            except FileNotFoundError:
                pass
            ctx = GLib.MainContext.default()
            while ctx.pending():
                ctx.iteration(False)
            time.sleep(0.5)
        return None

    def obtain_code(self):
        if self.code_file:
            print(f"pair: waiting for the 6-digit PIN in "
                  f"{self.code_file} ({self.wait_s}s)\u2026",
                  file=sys.stderr)
            return self._from_file()
        # Interactive: BlueZ abandons this agent call after ~25 s, so the
        # answer must land inside the SMP window anyway.
        print("pair: enter the 6-digit PIN shown on the SomnoBridge LCD "
              "(~25 s):", file=sys.stderr)
        try:
            raw = input("PIN> ").strip()
        except (EOFError, KeyboardInterrupt):
            return None
        return int(raw) if len(raw) == 6 and raw.isdigit() else None

    @dbus.service.method("org.bluez.Agent1", in_signature="o",
                         out_signature="u")
    def RequestPasskey(self, device):
        code = self.obtain_code()
        if code is None:
            print("pair: no valid PIN \u2014 rejecting", file=sys.stderr)
            raise dbus.exceptions.DBusException("org.bluez.Error.Rejected")
        return dbus.UInt32(code)

    @dbus.service.method("org.bluez.Agent1", in_signature="",
                         out_signature="")
    def Release(self):
        pass

    @dbus.service.method("org.bluez.Agent1", in_signature="o",
                         out_signature="s")
    def RequestPinCode(self, device):
        raise dbus.exceptions.DBusException(
            "org.bluez.Error.NotSupported")

    @dbus.service.method("org.bluez.Agent1", in_signature="ouq",
                         out_signature="")
    def DisplayPasskey(self, device, passkey, entered):
        # Our device is DisplayOnly; if BlueZ ever asks us to display,
        # surface it so the user knows what to type.
        print(f"pair: passkey {passkey:06d} (entered {entered})",
              file=sys.stderr)

    @dbus.service.method("org.bluez.Agent1", in_signature="ou",
                         out_signature="")
    def RequestConfirmation(self, device, passkey):
        # Not expected for a DisplayOnly peer; accept only in file mode
        # when the file already holds the same value (user-confirmed).
        if self.code_file:
            try:
                with open(self.code_file) as f:
                    if f.read().strip() == f"{passkey:06d}":
                        return
            except OSError:
                pass
        raise dbus.exceptions.DBusException("org.bluez.Error.Rejected")

    @dbus.service.method("org.bluez.Agent1", in_signature="os",
                         out_signature="")
    def AuthorizeService(self, device, uuid):
        pass    # our device, our pairing run


def main():
    ap = argparse.ArgumentParser(description="Pair this host with a "
                                 "SomnoBridge over BLE.")
    ap.add_argument("--address", help="device BLE address (default: scan "
                    "for the service UUID / name prefix)")
    ap.add_argument("--code-file", default=None,
                    help="scripted mode: poll this file for the 6-digit "
                    f"LCD PIN instead of prompting (default "
                    f"{DEFAULT_CODE_FILE} when given without a value)")
    ap.add_argument("--passkey-wait", type=int, default=PASSKEY_POLL_S,
                    help="seconds to wait for the PIN file (default "
                    f"{PASSKEY_POLL_S})")
    ap.add_argument("--re-pair", action="store_true",
                    help="remove the stale BlueZ pairing record first — "
                    "needed when the DEVICE side lost the bond (reflash, "
                    "NVS wipe, FORGET_PEER)")
    args = ap.parse_args()
    if args.code_file == "":
        args.code_file = DEFAULT_CODE_FILE

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()

    def device_proxy(address):
        path = f"/org/bluez/hci0/dev_{address.replace(':', '_')}"
        dev = dbus.Interface(bus.get_object("org.bluez", path),
                             "org.bluez.Device1")
        return dev, dbus.Interface(dev, "org.freedesktop.DBus.Properties")

    addr = args.address or find_device_address()
    if not addr:
        fail(f"no {ADV_NAME_PREFIX}* device found. Is it out of SETUP mode "
             "(BLE suspended there)? It advertises in normal operation.")
    print(f"pair: found device {addr}")
    dev, props = device_proxy(addr)

    if args.code_file:
        # A stale code from a previous attempt must not satisfy pairing.
        try:
            os.unlink(args.code_file)
        except FileNotFoundError:
            pass

    agent = Agent(bus, AGENT_PATH, args.code_file, args.passkey_wait)
    am = dbus.Interface(bus.get_object("org.bluez", "/org/bluez"),
                        "org.bluez.AgentManager1")
    am.RegisterAgent(AGENT_PATH, "KeyboardDisplay")
    am.RequestDefaultAgent(AGENT_PATH)

    result = {"paired": False, "err": None}
    loop = GLib.MainLoop()

    def done():
        try:
            am.UnregisterAgent(AGENT_PATH)
        except dbus.exceptions.DBusException:
            pass
        loop.quit()

    def on_paired():
        result["paired"] = True
        done()

    def on_error(e):
        result["err"] = e
        done()

    try:
        try:
            paired = bool(props.Get("org.bluez.Device1", "Paired"))
        except dbus.exceptions.DBusException:
            paired = False
        if paired and args.re_pair:
            print("pair: removing stale BlueZ record (device-side bond is "
                  "gone) \u2014 re-pairing fresh")
            adapter = dbus.Interface(
                bus.get_object("org.bluez", "/org/bluez/hci0"),
                "org.bluez.Adapter1")
            adapter.RemoveDevice(dev.object_path)
            # RemoveDevice destroys the Device1 object; the device must be
            # re-discovered (advertising) before we can pair with it again.
            addr = find_device_address(timeout=15)
            if not addr:
                fail("device not advertising after record removal \u2014 "
                     "complete SETUP first (BLE is suspended there), then "
                     "short-press for the pairing window and re-run")
            print(f"pair: rediscovered device {addr}")
            dev, props = device_proxy(addr)
            paired = False
        if paired:
            print("pair: already paired in BlueZ \u2014 if the device "
                  "refuses connections (its bond was wiped), re-run with "
                  "--re-pair")
        else:
            print("pair: connecting + pairing \u2014 watch the device LCD "
                  "for the PIN")
            dev.Pair(reply_handler=on_paired, error_handler=on_error,
                     timeout=PAIR_TIMEOUT_S)
            loop.run()
            if result["err"] is not None:
                raise result["err"]
        props.Set("org.bluez.Device1", "Trusted", dbus.Boolean(True))
        print("pair: paired + trusted \u2014 verify with "
              "./scripts/ble_query.py --status")
    except dbus.exceptions.DBusException as e:
        name = e.get_dbus_name()
        if "AlreadyExists" in name:
            props.Set("org.bluez.Device1", "Trusted", dbus.Boolean(True))
            print("pair: already paired + trusted")
        else:
            raise
    finally:
        try:
            am.UnregisterAgent(AGENT_PATH)
        except dbus.exceptions.DBusException:
            pass


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
    except dbus.exceptions.DBusException as e:
        name = e.get_dbus_name()
        if "AuthenticationCanceled" in name:
            fail("the device canceled the pairing. Most likely the pairing "
                 "window was not open \u2014 short-press the device button "
                 "FIRST (LCD shows PAIRING 120S), then run this script. "
                 "Also check the PIN was entered within ~25 s (3 failed "
                 "attempts close the window).")
        fail(f"BlueZ error: {name}\n  {e.get_dbus_message()}")
