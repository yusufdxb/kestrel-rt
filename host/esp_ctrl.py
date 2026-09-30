#!/usr/bin/env python3
"""UDP control of the Kestrel ESP32-S3 node (over its SoftAP).

    python3 host/esp_ctrl.py stats
    python3 host/esp_ctrl.py "fault malformed 10000 2000"
Library: Esp().cmd(text) -> reply str; Esp().stats() -> dict (None if the ESP does not answer).
"""
import socket
import sys

ESP_IP = "192.168.4.1"
CTRL_PORT = 47000
SINK_PORT = 47002


class Esp:
    def __init__(self, ip=ESP_IP, timeout=1.0):
        self.ip = ip
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.settimeout(timeout)

    def cmd(self, text, tries=3):
        for _ in range(tries):
            try:
                self.s.sendto(text.encode(), (self.ip, CTRL_PORT))
                while True:
                    d, _ = self.s.recvfrom(2048)
                    if not d.startswith(b"KX"):  # skip forwarded telemetry frames
                        return d.decode(errors="replace")
            except (socket.timeout, OSError):
                continue
        return None

    def stats(self):
        r = self.cmd("stats")
        if not r or not r.startswith("stats"):
            return None
        out = {}
        for kv in r.split()[1:]:
            k, v = kv.split("=", 1)
            out[k] = [int(x) for x in v.split(",")] if "," in v else int(v)
        return out


if __name__ == "__main__":
    e = Esp()
    print(e.stats() if sys.argv[1:] == ["stats"] else e.cmd(" ".join(sys.argv[1:])))
