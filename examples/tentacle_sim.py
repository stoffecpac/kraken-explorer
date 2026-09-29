#!/usr/bin/env python3
"""Simulate a kraken on a vcan interface, matching examples/tentacle.dbc.

Stdlib only (AF_CAN socket), runs outside Kraken Explorer so the app is the receiver.
Usage: ./tentacle_sim.py [iface]      (default vcan0; run scripts/setup_vcan.sh first, load
tentacle.dbc in Measurement > Setup to see the decoded signals)
"""
import math, socket, struct, sys, time

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

if __name__ == "__main__":
    # self-check: t=0, tentacle 0 -> angle 0, curl 50 %, grip 0, 100 suckers, Idle
    assert tentacle_frame(0, 0) == bytes([0, 0, 100, 0, 0, 100, 0, 0]), tentacle_frame(0, 0)
    try:
        main(*sys.argv[1:])
    except KeyboardInterrupt:
        pass
