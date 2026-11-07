#!/usr/bin/env python3
"""Developer tool for the Indygo-Pool-Web-API gateway (standard library only).

Examples
  python tools/indygo_dev.py status
  python tools/indygo_dev.py ota firmware_build/indygo_gateway.ino.bin
  python tools/indygo_dev.py ble-scan
  python tools/indygo_dev.py ble-connect aa:bb:cc:dd:ee:ff --type 1
  python tools/indygo_dev.py ble-services
  python tools/indygo_dev.py ble-read <service-uuid> <characteristic-uuid>
  python tools/indygo_dev.py ble-sub  <service-uuid> <characteristic-uuid>
  python tools/indygo_dev.py ble-log --follow
  python tools/indygo_dev.py ble-write-enable --minutes 10
  python tools/indygo_dev.py ble-write <service-uuid> <characteristic-uuid> 0204

The gateway is found at $INDYGO_HOST (default indygo-gateway.local). The developer password comes from
--password, $INDYGO_DEV_PASSWORD or the file ~/.indygo_dev_password. BLE writes are locked on the device until
you run ble-write-enable, and every write is logged there.
"""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
import uuid


def password(args):
    if args.password:
        return args.password
    if os.environ.get("INDYGO_DEV_PASSWORD"):
        return os.environ["INDYGO_DEV_PASSWORD"]
    path = os.path.expanduser("~/.indygo_dev_password")
    if os.path.exists(path):
        return open(path, encoding="utf-8").read().strip()
    return ""


def call(args, method, path, body=None, auth=True, data=None, headers=None, timeout=20):
    url = "http://%s/api/v1/%s" % (args.host, path)
    hdrs = dict(headers or {})
    if auth:
        hdrs["X-Dev-Password"] = password(args)
    if body is not None:
        data = json.dumps(body).encode()
        hdrs["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, method=method, headers=hdrs)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read().decode() or "null"
            return r.status, json.loads(raw)
    except urllib.error.HTTPError as e:
        raw = e.read().decode()
        try:
            return e.code, json.loads(raw)
        except ValueError:
            return e.code, {"error": raw}


def show(status, body):
    print(status, json.dumps(body, indent=2))
    return 0 if status < 300 else 1


def multipart(field, filename, content):
    boundary = uuid.uuid4().hex
    head = ('--%s\r\nContent-Disposition: form-data; name="%s"; filename="%s"\r\n'
            "Content-Type: application/octet-stream\r\n\r\n" % (boundary, field, filename)).encode()
    tail = ("\r\n--%s--\r\n" % boundary).encode()
    return {"Content-Type": "multipart/form-data; boundary=" + boundary}, head + content + tail


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("INDYGO_HOST", "indygo-gateway.local"))
    ap.add_argument("--password")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    sub.add_parser("demo-off")
    sub.add_parser("demo-on")
    p = sub.add_parser("ota"); p.add_argument("bin")
    p = sub.add_parser("set-password"); p.add_argument("new")
    sub.add_parser("ble-scan")
    p = sub.add_parser("ble-connect"); p.add_argument("address"); p.add_argument("--type", type=int, default=0)
    sub.add_parser("ble-disconnect")
    sub.add_parser("ble-services")
    for name in ("ble-read", "ble-sub"):
        p = sub.add_parser(name); p.add_argument("service"); p.add_argument("characteristic")
    p = sub.add_parser("ble-write"); p.add_argument("service"); p.add_argument("characteristic"); p.add_argument("hex")
    p.add_argument("--no-response", action="store_true")
    p = sub.add_parser("ble-write-enable"); p.add_argument("--minutes", type=int, default=10); p.add_argument("--off", action="store_true")
    p = sub.add_parser("ble-log"); p.add_argument("--since", type=int, default=0); p.add_argument("--follow", action="store_true")
    a = ap.parse_args()

    if a.cmd == "status":
        s1, g = call(a, "GET", "gateway", auth=False)
        s2, d = call(a, "GET", "dev", auth=False)
        print(json.dumps({"gateway": g, "dev": d}, indent=2))
        return 0
    if a.cmd in ("demo-on", "demo-off"):
        return show(*call(a, "PUT", "demo", {"enabled": a.cmd == "demo-on"}, auth=False))
    if a.cmd == "ota":
        content = open(a.bin, "rb").read()
        hdrs, data = multipart("file", os.path.basename(a.bin), content)
        print("uploading %d bytes ..." % len(content))
        return show(*call(a, "POST", "ota", data=data, headers=hdrs, timeout=180))
    if a.cmd == "set-password":
        return show(*call(a, "PUT", "dev/password", {"password": a.new, "current": password(a)}, auth=False))
    if a.cmd == "ble-scan":
        s, _ = call(a, "POST", "ble/scan", auth=False)
        time.sleep(10)
        s, d = call(a, "GET", "ble/scan", auth=False)
        for x in sorted(d.get("devices", []), key=lambda v: -v["rssi"]):
            print("%-4s %-18s %-5s %-14s %s" % (x["rssi"], x["address"], x.get("addrType", ""), x["family"], x["name"]))
        return 0
    if a.cmd == "ble-connect":
        return show(*call(a, "POST", "dev/ble/connect", {"address": a.address, "addrType": a.type}, timeout=40))
    if a.cmd == "ble-disconnect":
        return show(*call(a, "POST", "dev/ble/disconnect", {}))
    if a.cmd == "ble-services":
        return show(*call(a, "GET", "dev/ble/services"))
    if a.cmd == "ble-read":
        return show(*call(a, "POST", "dev/ble/read", {"service": a.service, "characteristic": a.characteristic}))
    if a.cmd == "ble-sub":
        return show(*call(a, "POST", "dev/ble/subscribe", {"service": a.service, "characteristic": a.characteristic}))
    if a.cmd == "ble-write-enable":
        return show(*call(a, "PUT", "dev/ble/write-enable", {"enabled": not a.off, "minutes": a.minutes}))
    if a.cmd == "ble-write":
        return show(*call(a, "POST", "dev/ble/write", {"service": a.service, "characteristic": a.characteristic,
                                                         "hex": a.hex.replace(" ", ""), "response": not a.no_response}))
    if a.cmd == "ble-log":
        since = a.since
        while True:
            s, d = call(a, "GET", "dev/ble/log?since=%d" % since)
            if s >= 300:
                return show(s, d)
            for e in d["entries"]:
                print("[%8d ms] %s %s %s" % (e["ms"], e["dir"], e["uuid"], e["hex"]))
                since = e["seq"]
            if not a.follow:
                return 0
            time.sleep(1.5)
    return 2


if __name__ == "__main__":
    sys.exit(main())
