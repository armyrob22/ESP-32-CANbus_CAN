#!/usr/bin/env python3
"""Reference decoder + fake sender for the KCAN protocol (see PROTOCOL.md).

  listen  - bind a UDP port, decode packets, print live stats, optionally
            write records to a CSV. Use it to prove the ESP32 -> WireGuard
            -> server2 path before the real logger exists.
  fake    - pretend to be the ESP32: send protocol-correct packets to a
            host:port, so the server2 logger can be built and tested
            without the car. Supports --loss to simulate dropped packets.

stdlib only.
"""
import argparse
import csv
import random
import signal
import socket
import struct
import sys
import time

MAGIC = 0x4E41434B
VERSION = 1
PKT_FRAMES, PKT_STATUS = 1, 2

HDR = struct.Struct("<IBBHIIII")            # 24 bytes
REC = struct.Struct("<QIBBH8s")             # 24 bytes
STATUS = struct.Struct("<QQIIIIIIIIBBbBI")  # 56 bytes
assert HDR.size == 24 and REC.size == 24 and STATUS.size == 56

ID_MASK = 0x1FFFFFFF
ID_EXT, ID_RTR, ID_MARK = 1 << 29, 1 << 30, 1 << 31
RF_TIME_VALID, RF_KEEPALIVE, RF_SIM = 1, 2, 4
STATUS_FIELDS = ("ts_us", "uptime_us", "frames_rx", "records_queued",
                 "records_dropped", "twai_rx_missed", "twai_bus_errors",
                 "ring_used", "ring_capacity", "free_heap", "twai_state",
                 "twai_rx_err", "wifi_rssi", "time_valid", "marks")


def decode(pkt):
    """Return (header dict, list of records | status dict). Raises ValueError."""
    if len(pkt) < HDR.size:
        raise ValueError("short packet")
    magic, ver, typ, count, seq, boot_id, _, _ = HDR.unpack_from(pkt)
    if magic != MAGIC:
        raise ValueError("bad magic")
    if ver != VERSION:
        raise ValueError(f"unsupported version {ver}")
    hdr = {"type": typ, "count": count, "seq": seq, "boot_id": boot_id}
    body = pkt[HDR.size:]
    if typ == PKT_FRAMES:
        if len(body) != count * REC.size:
            raise ValueError("length/count mismatch")
        recs = []
        for i in range(count):
            ts, rid, dlc, rf, sup, data = REC.unpack_from(body, i * REC.size)
            recs.append({
                "ts_us": ts,
                "id": rid & ID_MASK,
                "ext": bool(rid & ID_EXT),
                "rtr": bool(rid & ID_RTR),
                "mark": bool(rid & ID_MARK),
                "dlc": dlc,
                "data": data[:min(dlc, 8)],
                "time_valid": bool(rf & RF_TIME_VALID),
                "keepalive": bool(rf & RF_KEEPALIVE),
                "sim": bool(rf & RF_SIM),
                "suppressed": sup,
            })
        return hdr, recs
    if typ == PKT_STATUS:
        if len(body) != STATUS.size:
            raise ValueError("bad status length")
        return hdr, dict(zip(STATUS_FIELDS, STATUS.unpack(body)))
    raise ValueError(f"unknown type {typ}")


class SeqTracker:
    """Per-boot gap counting. seq is shared by FRAMES and STATUS packets."""
    def __init__(self):
        self.boot_id = None
        self.expected = None
        self.received = 0
        self.lost = 0
        self.reordered = 0

    def feed(self, boot_id, seq):
        if boot_id != self.boot_id:
            self.boot_id, self.expected = boot_id, seq + 1
            self.received += 1
            return "new_boot"
        self.received += 1
        if seq == self.expected:
            self.expected += 1
        elif seq > self.expected:
            self.lost += seq - self.expected
            self.expected = seq + 1
        else:
            self.reordered += 1   # late packet; counted lost earlier
            self.lost = max(0, self.lost - 1)
        return None


def cmd_listen(a):
    # Treat SIGTERM (systemd stop, timeout) like Ctrl-C so the CSV gets closed.
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt))
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((a.bind, a.port))
    sock.settimeout(1.0)
    out = None
    if a.csv:
        f = open(a.csv, "w", newline="")
        out = csv.writer(f)
        out.writerow(["ts_us", "time_valid", "kind", "id_hex", "ext", "dlc",
                      "data_hex", "keepalive", "suppressed", "boot_id", "seq"])
    trk = SeqTracker()
    n_recs = n_marks = bad = 0
    last_status = None
    last_print = time.time()
    print(f"listening on {a.bind}:{a.port}")
    try:
        while True:
            try:
                pkt, src = sock.recvfrom(2048)
            except socket.timeout:
                pkt = None
            if pkt:
                try:
                    hdr, body = decode(pkt)
                except ValueError as e:
                    bad += 1
                    if a.verbose:
                        print(f"bad packet from {src}: {e}")
                    continue
                if trk.feed(hdr["boot_id"], hdr["seq"]) == "new_boot":
                    print(f"device boot_id {hdr['boot_id']:08x} from {src[0]}")
                if hdr["type"] == PKT_STATUS:
                    st = body
                    if a.verbose:
                        print("status", st)
                    last_status = st
                else:
                    for r in body:
                        n_recs += 1
                        if r["mark"]:
                            n_marks += 1
                            print(f"MARK #{r['id']} at {r['ts_us']}")
                        if out:
                            out.writerow([r["ts_us"], int(r["time_valid"]),
                                          "mark" if r["mark"] else "frame",
                                          f"{r['id']:X}", int(r["ext"]), r["dlc"],
                                          r["data"].hex(), int(r["keepalive"]),
                                          r["suppressed"], f"{hdr['boot_id']:08x}",
                                          hdr["seq"]])
            if time.time() - last_print >= a.interval:
                last_print = time.time()
                if out:
                    f.flush()
                s = last_status
                dev = ""
                if s:
                    dev = (f" | dev rx={s['frames_rx']} drop={s['records_dropped']}"
                           f" buf={s['ring_used']}/{s['ring_capacity']}"
                           f" rssi={s['wifi_rssi']} twai={s['twai_state']}")
                print(f"pkts={trk.received} lost={trk.lost} recs={n_recs}"
                      f" marks={n_marks} bad={bad}{dev}")
    except KeyboardInterrupt:
        pass
    finally:
        if out:
            f.close()


def cmd_fake(a):
    """Emit what the firmware emits: 4 IDs, change-only filtered, marks every
    --mark-every seconds, status every second."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dst = (a.host, a.port)
    boot_id = random.getrandbits(32)
    seq = 0
    pending = []
    slots = {}
    mark_no = 0
    t0 = time.time()
    next_status = next_mark = t0 + 1
    next_flush = t0
    tick = 0
    sent = dropped = 0

    def send(typ, payload, count):
        nonlocal seq, sent, dropped
        pkt = HDR.pack(MAGIC, VERSION, typ, count, seq, boot_id, 0, 0) + payload
        seq += 1
        if random.random() < a.loss:
            dropped += 1
            return
        sock.sendto(pkt, dst)
        sent += 1

    def frame(cid, data, now_us):
        prev = slots.get(cid)
        changed = prev is None or prev[0] != data
        if not changed and now_us - prev[1] < 1_000_000:
            slots[cid] = (prev[0], prev[1], prev[2] + 1)
            return
        sup = 0 if prev is None else prev[2]
        rf = RF_TIME_VALID | RF_SIM | (0 if changed else RF_KEEPALIVE)
        pending.append(REC.pack(now_us, cid, len(data), rf, min(sup, 65535),
                                data.ljust(8, b"\0")))
        slots[cid] = (data, now_us, 0)

    print(f"fake device boot_id {boot_id:08x} -> {a.host}:{a.port}"
          f" (loss {a.loss:.0%}), Ctrl-C to stop")
    try:
        while a.seconds <= 0 or time.time() - t0 < a.seconds:
            tick += 1
            now = time.time()
            now_us = int(now * 1e6)
            if tick % 10 == 0:
                rpm = 750 + (tick // 100) % 400
                frame(0x316, bytes([0, 0, rpm & 0xFF, rpm >> 8, 0, 0, 0, 0]), now_us)
            if tick % 10 == 5:
                frame(0x329, bytes([tick // 10 & 0x0F, 0, 0, 0, 0, 0, 0, 0]), now_us)
            if tick % 100 == 0:
                frame(0x541, bytes([0, 1 if (tick // 5000) & 1 else 0, 0, 0, 0, 0, 0, 0]), now_us)
            if tick % 1000 == 0:
                frame(0x5F0, bytes([0xAA, 0x55, 0, 0]), now_us)
            if a.mark_every > 0 and now >= next_mark:
                mark_no += 1
                pending.append(REC.pack(now_us, ID_MARK | mark_no, 0, RF_TIME_VALID, 0,
                                        struct.pack("<I", mark_no).ljust(8, b"\0")))
                next_mark = now + a.mark_every
            while len(pending) >= 50 or (pending and now >= next_flush):
                batch, pending[:] = pending[:50], pending[50:]
                send(PKT_FRAMES, b"".join(batch), len(batch))
                next_flush = now + 0.05
            if now >= next_status:
                st = STATUS.pack(now_us, int((now - t0) * 1e6), tick, tick, 0, 0, 0,
                                 len(pending), 4096, 150000, 0xFF, 0, -55, 1, mark_no)
                send(PKT_STATUS, st, 0)
                next_status = now + 1
            time.sleep(0.001)
    except KeyboardInterrupt:
        pass
    print(f"sent={sent} dropped_on_purpose={dropped} last_seq={seq - 1}")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    l = sub.add_parser("listen")
    l.add_argument("--bind", default="0.0.0.0")
    l.add_argument("--port", type=int, default=47000)
    l.add_argument("--csv", help="write every record to this CSV")
    l.add_argument("--interval", type=float, default=2.0)
    l.add_argument("-v", "--verbose", action="store_true")
    f = sub.add_parser("fake")
    f.add_argument("--host", default="127.0.0.1")
    f.add_argument("--port", type=int, default=47000)
    f.add_argument("--loss", type=float, default=0.0, help="fraction of packets to drop")
    f.add_argument("--mark-every", type=float, default=5.0)
    f.add_argument("--seconds", type=float, default=0, help="0 = run until Ctrl-C")
    a = p.parse_args()
    {"listen": cmd_listen, "fake": cmd_fake}[a.cmd](a)


if __name__ == "__main__":
    main()
