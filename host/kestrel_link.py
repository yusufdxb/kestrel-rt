#!/usr/bin/env python3
"""Kestrel host link: frame codec, decoders for every target->host frame, and a small CLI.

    python3 host/kestrel_link.py status [--secs N]      print decoded STATUS frames
    python3 host/kestrel_link.py record OUT.bin --secs N raw capture (decode later with `decode`)
    python3 host/kestrel_link.py decode IN.bin           summarize a raw capture
    python3 host/kestrel_link.py cmd NAME [args...]      send one command, print replies

The serial port is always opened directly at 460800 baud. Never 1200 (the bridge treats it as a
bootloader-reset request) and never 2400 (ROM boot-mode request).
"""
import argparse
import glob
import os
import struct
import sys
import time

MAGIC = b"KR"
VERSION = 1
KF = {0x01: "HELLO", 0x02: "STATUS", 0x03: "JOBS", 0x04: "TRACE", 0x05: "CRASH", 0x06: "ACK",
      0x07: "TASKS", 0x08: "RESULT", 0x40: "CMD"}
CMD = {"ping": 0x01, "get_crash": 0x02, "clear_crash": 0x03, "set_trace": 0x04, "get_tasks": 0x05,
       "tx_stall": 0x06, "time_sync": 0x07, "reset": 0x08, "get_hist": 0x09, "reset_stats": 0x0A,
       "trace_arm": 0x0B, "trace_stream": 0x0C, "set_work": 0x10, "inject": 0x20, "app_stats": 0x21, "storm": 0x22, "mpu": 0x23,
       "mutex_demo": 0x24}
TE = {1: "RELEASE", 2: "START", 3: "END", 4: "SWITCH", 5: "ISR_ENTER", 6: "ISR_EXIT",
      7: "DEADLINE_MISS", 8: "OVERRUN", 9: "QUEUE_OVERFLOW", 10: "WD_WARNING", 11: "FAULT",
      12: "RESET_REASON", 13: "BLOCK", 14: "WAKE", 15: "MUTEX", 16: "USER", 17: "DEGRADED", 18: "CMD"}
FAULT = {1: "HardFault", 2: "MemManage", 3: "BusFault", 4: "UsageFault", 5: "NMI", 6: "Assert",
         7: "TaskExit", 8: "WatchdogStall", 9: "WdtNmi", 11: "StackCanary", 12: "Kernel"}
RST = {0x1: "power-on", 0x2: "WDT", 0x4: "IWDT", 0x8: "software", 0x10: "LVD", 0x20: "SP-error",
       0x40: "RAM-error", 0x80: "bus-error", 0x100: "warm", 0x200: "pin/unknown"}
TICK_HZ = 48_000_000


def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode(ftype, payload, seq=0):
    body = struct.pack("<BBHH", VERSION, ftype, seq & 0xFFFF, len(payload)) + payload
    return MAGIC + body + struct.pack("<H", crc16(body))


class Decoder:
    """Mirror of trace/frame.c: byte-oriented, resynchronizes on magic."""

    def __init__(self):
        self.buf = bytearray()
        self.ok = 0
        self.bad = 0

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(MAGIC)
            if i < 0:
                self.buf = self.buf[-1:]
                return out
            if i:
                del self.buf[:i]
            if len(self.buf) < 8:
                return out
            ver, ftype, seq, ln = struct.unpack_from("<BBHH", self.buf, 2)
            if ver != VERSION or ln > 480:
                self.bad += 1
                del self.buf[:2]
                continue
            if len(self.buf) < 10 + ln:
                return out
            body = bytes(self.buf[2:8 + ln])
            (crc,) = struct.unpack_from("<H", self.buf, 8 + ln)
            if crc16(body) != crc:
                self.bad += 1
                del self.buf[:2]
                continue
            out.append((ftype, seq, bytes(self.buf[8:8 + ln])))
            self.ok += 1
            del self.buf[:10 + ln]


def parse_status(p):
    names = ["uptime_lo", "uptime_hi", "boot_count", "reset_cause", "ctx_switches", "alarms", "svcs",
             "max_alarm_cyc", "max_sched_cyc", "max_crit_cyc", "max_pendsv_cyc", "trace_events",
             "trace_dropped", "jobs", "jobs_dropped", "rx_bytes", "rx_errors", "tx_bytes",
             "rx_queue_rejected", "cmd_frames_ok", "cmd_frames_bad", "dwt_rearms", "degraded",
             "wd_warnings", "wdt_feeds", "faults", "tx_ring_full", "ntasks"]
    v = struct.unpack_from("<" + "I" * len(names), p, 0)
    s = dict(zip(names, v))
    s["uptime_s"] = (s["uptime_hi"] << 32 | s["uptime_lo"]) / TICK_HZ
    o = 4 * len(names)
    tasks = []
    while o + 40 <= len(p):
        tid, st, stk = struct.unpack_from("<BBH", p, o)
        f = struct.unpack_from("<9I", p, o + 4)
        tasks.append({"id": tid, "state": st, "stack_used": stk, "jobs": f[0], "misses": f[1],
                      "overruns": f[2], "wcet_cyc": f[3], "max_resp_ticks": f[4],
                      "max_start_lat_ticks": f[5], "faults": f[6], "run_cycles": f[7] | f[8] << 32})
        o += 40
    s["tasks"] = tasks
    return s


def parse_jobs(p):
    return [struct.unpack_from("<BBHHHHH", p, i) for i in range(0, len(p) - 11, 12)]


def parse_trace(p):
    out = []
    for i in range(0, len(p) - 7, 8):
        ts, ev, data = struct.unpack_from("<IHH", p, i)
        out.append((ts, ev >> 8, ev & 0xFF, data))
    return out


CRASH_FIELDS = ["magic", "version", "size", "kind", "task", "flags", "boot_count", "time_lo", "time_hi",
                "r0", "r1", "r2", "r3", "r12", "lr", "pc", "xpsr", "cfsr", "hfsr", "mmfar", "bfar",
                "shcsr", "icsr", "exc_return", "sp", "detail", "stack_used", "capture_cycles", "crc"]
CRASH_FMT = "<IHHHBBIII8I6IIIIHHI"


def parse_crash(p):
    boot, cause, present = struct.unpack_from("<IIB", p, 0)
    rec = dict(zip(CRASH_FIELDS, struct.unpack_from(CRASH_FMT, p, 9)))
    return {"boot_count": boot, "reset_cause": cause, "reset_cause_s": reset_str(cause),
            "present": bool(present), "record": rec}


def parse_tasks(p):
    out = []
    for i in range(0, len(p) - 27, 28):
        tid, prio, flags, _, per, dl, stk = struct.unpack_from("<BBBBIII", p, i)
        name = p[i + 16:i + 28].split(b"\0")[0].decode(errors="replace")
        out.append({"id": tid, "prio": prio, "flags": flags, "period_us": per, "deadline_us": dl,
                    "stack": stk, "name": name})
    return out


def reset_str(c):
    return "+".join(n for b, n in RST.items() if c & b) or "none"


def find_port():
    for d in sorted(glob.glob("/sys/class/tty/ttyACM*")):
        u = os.path.join(d, "device", "..")
        try:
            vid = open(os.path.join(u, "idVendor")).read().strip()
            pid = open(os.path.join(u, "idProduct")).read().strip()
        except OSError:
            continue
        if vid == "2341" and pid == "1002":
            return "/dev/" + os.path.basename(d)
    raise SystemExit("UNO R4 WiFi bridge not found")


def open_port(port=None):
    import serial
    return serial.Serial(port or find_port(), 460800, timeout=0.05)


class Link:
    def __init__(self, port=None):
        self.ser = open_port(port)
        self.dec = Decoder()
        self.seq = 0
        self.raw = bytearray()

    def send(self, cmd, payload=b""):
        try:
            self._send(cmd, payload)
        except Exception:  # noqa: BLE001
            try:
                self.reopen()
                self._send(cmd, payload)
            except Exception:  # noqa: BLE001
                pass

    def _send(self, cmd, payload=b""):
        if isinstance(cmd, str):
            code = CMD[cmd] if cmd in CMD else int(cmd, 0)
        else:
            code = cmd
        self.ser.write(encode(0x40, bytes([code]) + payload, self.seq))
        self.seq += 1

    def reopen(self):
        try:
            self.ser.close()
        except Exception:  # noqa: BLE001
            pass
        time.sleep(1.0)
        self.ser = open_port()

    def poll(self, secs=0.1):
        out = []
        end = time.time() + secs
        while time.time() < end:
            try:
                d = self.ser.read(8192)
            except Exception:  # noqa: BLE001  (bridge re-enumerated under us)
                time.sleep(1.0)
                try:
                    self.reopen()
                except Exception:  # noqa: BLE001
                    pass
                continue
            if d:
                self.raw += d
                out += self.dec.feed(d)
        return out

    def wait_for(self, ftype, secs=3.0, pred=None):
        end = time.time() + secs
        while time.time() < end:
            for f in self.poll(0.05):
                if f[0] == ftype and (pred is None or pred(f)):
                    return f
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--secs", type=float, default=3.0)
    a = ap.parse_args()
    if a.action == "status":
        lk = Link()
        end = time.time() + a.secs
        while time.time() < end:
            for t, s, p in lk.poll(0.2):
                if t == 0x02:
                    st = parse_status(p)
                    print({k: v for k, v in st.items() if k != "tasks"})
                    for tk in st["tasks"]:
                        print("   ", tk)
        print("frames ok", lk.dec.ok, "bad", lk.dec.bad)
    elif a.action == "record":
        lk = Link()
        with open(a.args[0], "wb") as f:
            end = time.time() + a.secs
            while time.time() < end:
                d = lk.ser.read(16384)
                if d:
                    f.write(d)
    elif a.action == "decode":
        dec = Decoder()
        counts = {}
        for t, s, p in dec.feed(open(a.args[0], "rb").read()):
            counts[KF.get(t, t)] = counts.get(KF.get(t, t), 0) + 1
        print(counts, "ok", dec.ok, "bad", dec.bad)
    elif a.action == "cmd":
        lk = Link()
        name = a.args[0]
        payload = bytes(int(x, 0) & 0xFF for x in a.args[1:])
        lk.send(name, payload)
        for t, s, p in lk.poll(a.secs):
            if t == 0x05:
                print("CRASH", parse_crash(p))
            elif t == 0x06:
                print("ACK", p.hex())
            elif t == 0x07:
                for x in parse_tasks(p):
                    print("TASK", x)
            elif t == 0x08:
                print("RESULT", p.hex())
    else:
        ap.error("unknown action")


if __name__ == "__main__":
    main()
