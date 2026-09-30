#!/usr/bin/env python3
"""ESP32-S3 bridge health: the UNO R4 WiFi's USB bridge occasionally stops delivering CDC data
(observed after host read stalls and around RA4 resets). Recovery used by the test drivers:
one hub-port disable/enable, only while the RA4M1 runs the application (never while it sits in
the bootloader, see ~/.claude memory reference_uno_r4_native_usb). Every recovery is logged."""
import glob
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kestrel_link as kl  # noqa: E402

LOG = Path(__file__).resolve().parents[1] / "results" / "bridge_recoveries.log"


def link_alive(secs=3.0):
    try:
        lk = kl.Link()
    except Exception:  # noqa: BLE001
        return False
    try:
        return any(t == 0x02 or t == 0x09 for t, s, p in lk.poll(secs))
    finally:
        lk.ser.close()


def port_path():
    for d in glob.glob("/sys/bus/usb/devices/*"):
        try:
            if open(d + "/idVendor").read().strip() == "2341" and open(d + "/idProduct").read().strip() == "1002":
                name = os.path.basename(d)  # e.g. 1-3
                bus, port = name.split("-")[0], name.split("-")[-1]
                return f"/sys/bus/usb/devices/usb{bus}/{bus}-0:1.0/usb{bus}-port{port}"
        except OSError:
            continue
    return None


def app_running():
    from pyocd.core.helpers import ConnectHelper
    opts = {"connect_mode": "attach", "resume_on_disconnect": True}
    with ConnectHelper.session_with_chosen_probe(target_override="r7fa4m1ab", options=opts) as s:
        return s.target.read32(0xE000ED08) == 0x4000 and s.target.read32(0x4001E500) != 0x07738135


MIN_CYCLE_INTERVAL_S = 600  # repeated port cycles wedged the ESP32 (2026-09-29 21:14): rate-limit


def last_cycle_time():
    try:
        for line in reversed(LOG.read_text().splitlines()):
            if "hub-port cycle" in line:
                return time.mktime(time.strptime(line.split()[0], "%Y-%m-%dT%H:%M:%SZ")) - time.timezone
    except (OSError, ValueError):
        pass
    return 0.0


def ensure_link(reason=""):
    if link_alive():
        return True
    if time.time() - last_cycle_time() < MIN_CYCLE_INTERVAL_S:
        with open(LOG, "a") as f:
            f.write(f"{time.strftime('%FT%TZ', time.gmtime())} link dead, cycle refused (rate limit) {reason}\n")
        return False
    p = port_path()
    if p is None or not app_running():
        with open(LOG, "a") as f:
            f.write(f"{time.strftime('%FT%TZ', time.gmtime())} link dead, NOT cycling (port={p}) {reason}\n")
        return False
    subprocess.run(["sudo", "-n", "/usr/bin/tee", p + "/disable"], input=b"1", stdout=subprocess.DEVNULL, check=True)
    time.sleep(3)
    subprocess.run(["sudo", "-n", "/usr/bin/tee", p + "/disable"], input=b"0", stdout=subprocess.DEVNULL, check=True)
    for _ in range(40):
        time.sleep(1)
        if glob.glob("/dev/ttyACM*"):
            break
    time.sleep(2)
    for n in glob.glob("/dev/ttyACM*") + glob.glob("/dev/hidraw*"):
        subprocess.run(["sudo", "-n", "/usr/bin/chmod", "666", n], check=False)
    ok = link_alive()
    with open(LOG, "a") as f:
        f.write(f"{time.strftime('%FT%TZ', time.gmtime())} hub-port cycle -> link {'OK' if ok else 'STILL DEAD'} {reason}\n")
    return ok


if __name__ == "__main__":
    print("link", "OK" if ensure_link("manual") else "DEAD")
