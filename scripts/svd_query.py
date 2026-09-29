#!/usr/bin/env python3
"""Look up RA4M1 peripheral registers/fields in the vendor SVD (not vendored; read from the local
Arduino renesas_uno install). Usage: svd_query.py PERIPH [REGISTER_SUBSTRING]"""
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

SVD = Path.home() / ".arduino15/packages/arduino/hardware/renesas_uno/1.6.0/svd/R7FA4M1AB.svd"


def main():
    root = ET.parse(SVD).getroot()
    periphs = {p.findtext("name"): p for p in root.iter("peripheral")}
    p = periphs[sys.argv[1]]
    base_p = p
    if p.get("derivedFrom"):
        base_p = periphs[p.get("derivedFrom")]
    base = int(p.findtext("baseAddress"), 16)
    want = sys.argv[2].upper() if len(sys.argv) > 2 else ""
    for r in base_p.iter("register"):
        name = r.findtext("name")
        if want and want not in name.upper():
            continue
        off = int(r.findtext("addressOffset"), 16)
        size = r.findtext("size") or "?"
        rst = r.findtext("resetValue") or "?"
        print(f"{name:14s} @0x{base + off:08X} size {size} reset {rst}  {(r.findtext('description') or '').strip()[:70]}")
        if want:
            for f in r.iter("field"):
                br = f.findtext("bitRange")
                if f.findtext("lsb") is not None:
                    lsb = int(f.findtext("lsb")); msb = int(f.findtext("msb"))
                elif br:
                    msb, lsb = (int(x) for x in br.strip("[]").split(":"))
                else:
                    lsb = int(f.findtext("bitOffset")); msb = lsb + int(f.findtext("bitWidth")) - 1
                print(f"    {f.findtext('name'):12s} [{msb}:{lsb}] {(f.findtext('description') or '').strip()[:90]}")


if __name__ == "__main__":
    main()
