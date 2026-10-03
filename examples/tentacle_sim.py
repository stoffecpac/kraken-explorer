#!/usr/bin/env python3
"""Simulate a kraken on a vcan interface, matching examples/tentacle.dbc.

Stdlib only (AF_CAN socket), runs outside Kraken Explorer so the app is the receiver.
Usage: ./tentacle_sim.py [iface]      (default vcan0; run scripts/setup_vcan.sh first, load
tentacle.dbc in Measurement > Setup to see the decoded signals)
       ./tentacle_sim.py --file big.log --size 8G [--format candump|asc]
                                       (offline: simulated time, no socket, stops at --size)
"""
import argparse, io, math, socket, struct, sys, time

STATUS_ID = 0x100          # KrakenStatus, 200 ms
TENTACLE_ID = 0x111        # Tentacle1..8 = 0x111..0x118, 50 ms each
N_TENTACLES = 8
PERIOD = 0.05

def tentacle_frame(n, t):
    """Raw payload for tentacle n at time t: wave with per-tentacle phase offset."""
    ph = n * 2 * math.pi / N_TENTACLES
    angle = 150 * math.sin(0.5 * t + ph)                      # deg
    curl = 50 + 50 * math.sin(0.8 * t + ph)                   # %
    grip = max(0.0, 300 * math.sin(0.3 * t + ph))             # N, only when curled
    state = 2 if grip > 200 else 1 if grip > 0 else 0
    suckers = int(200 * curl / 100)
    return struct.pack("<hBHBB", round(angle / 0.01), round(curl / 0.5), round(grip / 0.1), suckers, state) + b"\0"

def status_frame(t, counter):
    depth = 1000 + 800 * math.sin(0.05 * t)                   # m
    hr = 20 + 10 * math.sin(0.2 * t)                          # bpm
    ink = max(0.0, 100 - (t % 120))                           # % drains, refills every 2 min
    mood = int(t / 10) % 5
    return struct.pack("<HBBB", round(depth / 0.1), round(hr), round(ink / 0.5), mood | (counter << 4)) + b"\0\0\0"

def main(iface="vcan0"):
    s = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    s.bind((iface,))
    t0 = time.monotonic()
    tick = 0
    while True:
        t = time.monotonic() - t0
        for n in range(N_TENTACLES):
            s.send(struct.pack("=IB3x8s", TENTACLE_ID + n, 8, tentacle_frame(n, t)))
        if tick % 4 == 0:
            s.send(struct.pack("=IB3x8s", STATUS_ID, 8, status_frame(t, (tick // 4) & 0xF)))
        tick += 1
        time.sleep(max(0.0, t0 + tick * PERIOD - time.monotonic()))

def frames(t, tick):
    """(id, payload) of one 50 ms tick, as main() sends them."""
    out = [(TENTACLE_ID + n, tentacle_frame(n, t)) for n in range(N_TENTACLES)]
    if tick % 4 == 0:
        out.append((STATUS_ID, status_frame(t, (tick // 4) & 0xF)))
    return out

def write_log(path, size, fmt="candump", start=1700000000.0):
    """Writes ticks of simulated time to path until it holds at least size bytes."""
    with io.BufferedWriter(io.FileIO(path, "w"), 1 << 20) as out:
        if fmt == "asc":
            out.write(b"date Tue Nov 14 22:13:20.000 2023\nbase hex  timestamps absolute\n")
        written, tick = 0, 0
        while written < size:
            t = tick * PERIOD
            if fmt == "asc":
                lines = "".join(f"{t:11.6f} 1  {i:X}             Rx   d 8 {d.hex(' ').upper()}\n" for i, d in frames(t, tick))
            else:
                lines = "".join(f"({start + t:.6f}) vcan0 {i:03X}#{d.hex().upper()}\n" for i, d in frames(t, tick))
            written += out.write(lines.encode())
            tick += 1

def parse_size(s):
    return int(float(s[:-1]) * 1024 ** "KMGT".index(s[-1].upper()) * 1024) if s[-1].isalpha() else int(s)

if __name__ == "__main__":
    # self-check: t=0, tentacle 0 -> angle 0, curl 50 %, grip 0, 100 suckers, Idle
    assert tentacle_frame(0, 0) == bytes([0, 0, 100, 0, 0, 100, 0, 0]), tentacle_frame(0, 0)
    assert parse_size("8G") == 8 << 30 and parse_size("200M") == 200 << 20 and parse_size("123") == 123
    if "--file" in sys.argv:
        ap = argparse.ArgumentParser()
        ap.add_argument("--file", required=True)
        ap.add_argument("--size", default="200M")
        ap.add_argument("--format", choices=["candump", "asc"], default="candump")
        a = ap.parse_args()
        write_log(a.file, parse_size(a.size), a.format)
        sys.exit()
    try:
        main(*sys.argv[1:])
    except KeyboardInterrupt:
        pass
