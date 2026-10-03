#!/usr/bin/env python3
"""Writes the reference logs used by tests/replay with readers/writers that share no code with
Kraken Explorer (python-can 4.6.1, asammdf 8.8.27, see tests/format_compat/requirements.txt):

  python_can.mf4        python-can MF4Writer: ASAM bus logging (CAN_DataFrame / CAN_ErrorFrame /
                        CAN_RemoteFrame groups)
  python_can_dz.mf4     the same, saved by asammdf with transposed deflate (DZ) data blocks
  asammdf_lin.mf4       asammdf: LIN_Frame and LIN_ChecksumError bus logging groups
  asammdf_vlsd.mf4      asammdf: CAN_DataFrame.* channels with DataBytes as a variable length
                        (VLSD) channel, its SD block holding u32 length + bytes per frame
  asammdf_vlsd_dz.mf4   the same with compression=2 (the SD block becomes a DZ block)
  the BLF fixture       embedded in replay_test.cpp (python-can BLFWriter, zlib containers)

Usage: gen_ref.py <out dir>
"""
import datetime
import os
import sys

import can
import numpy as np
from asammdf import MDF, Signal, Source
from asammdf.blocks import v4_constants as v4c

MSGS = [
    can.Message(timestamp=1700000000.000000, arbitration_id=0x123, data=b"\x01\x02\x03", channel=0),
    can.Message(timestamp=1700000000.010000, arbitration_id=0x1ABCDEF0, is_extended_id=True, data=bytes(range(8)), channel=1),
    can.Message(timestamp=1700000000.020000, arbitration_id=0x7FF, is_remote_frame=True, dlc=4, channel=0),
    can.Message(timestamp=1700000000.030000, arbitration_id=0x456, is_fd=True, bitrate_switch=True, data=bytes(range(20)), channel=0),
    can.Message(timestamp=1700000000.040000, arbitration_id=0x0, is_error_frame=True, channel=1),
    can.Message(timestamp=1700000000.050000, arbitration_id=0x321, is_rx=False, data=b"\xAA", channel=0),
]

START = datetime.datetime(2023, 11, 14, 22, 13, 20, tzinfo=datetime.timezone.utc)  # 1700000000

out = sys.argv[1]
for name in ("python_can.mf4", "python_can.blf"):
    with can.Logger(os.path.join(out, name)) as w:
        for m in MSGS:
            w.on_message_received(m)
MDF(os.path.join(out, "python_can.mf4")).save(os.path.join(out, "python_can_dz.mf4"), compression=2, overwrite=True)


def lin_group(name, rows):
    """A structure composition named `name` whose members are the ASAM LIN bus logging channels."""
    dt = np.dtype([(f"{name}.BusChannel", "<u1"), (f"{name}.ID", "<u1"), (f"{name}.DLC", "<u1"),
                   (f"{name}.DataLength", "<u1"), (f"{name}.DataBytes", "<u1", (8,)), (f"{name}.Dir", "<u1")])
    s = np.zeros(len(rows), dtype=dt)
    for i, (t, ch, id_, data, tx) in enumerate(rows):
        s[i] = (ch, id_, len(data), len(data), list(data) + [0] * (8 - len(data)), 1 if tx else 0)
    src = Source(name="LIN", path="LIN", comment="", source_type=v4c.SOURCE_BUS, bus_type=v4c.BUS_TYPE_LIN)
    return Signal(samples=s, timestamps=np.array([r[0] for r in rows]), name=name, source=src), src


m = MDF(version="4.10")
m.header.start_time = START
for sig, src in (lin_group("LIN_Frame", [(0.00, 1, 0x3C, bytes(range(8)), False),
                                         (0.01, 1, 0x10, b"\xAA\xBB", True),
                                         (0.02, 2, 0x3F, b"\x01\x02\x03\x04", False)]),
                 lin_group("LIN_ChecksumError", [(0.03, 1, 0x22, b"\x09\x08\x07", False)])):
    m.append([sig], acq_name="LIN", acq_source=src)
m.save(os.path.join(out, "asammdf_lin.mf4"), overwrite=True)

# compact_vlsd writes each DataBytes sample with its own length (numpy drops trailing NULs of an
# S8 element, so no payload here ends in 0x00).
t = np.array([0.0, 0.01, 0.02])
src = Source(name="CAN", path="CAN", comment="", source_type=v4c.SOURCE_BUS, bus_type=v4c.BUS_TYPE_CAN)
m = MDF(version="4.10", compact_vlsd=True)
m.header.start_time = START
m.append([
    Signal(np.array([1, 1, 2], dtype="<u1"), t, name="CAN_DataFrame.BusChannel", source=src),
    Signal(np.array([0x123, 0x1ABCDEF0 | 0x80000000, 0x7FF], dtype="<u4"), t, name="CAN_DataFrame.ID", source=src),
    Signal(np.array([0, 1, 0], dtype="<u1"), t, name="CAN_DataFrame.IDE", source=src),
    Signal(np.array([3, 8, 1], dtype="<u1"), t, name="CAN_DataFrame.DLC", source=src),
    Signal(np.array([3, 8, 1], dtype="<u1"), t, name="CAN_DataFrame.DataLength", source=src),
    Signal(np.array([0, 0, 1], dtype="<u1"), t, name="CAN_DataFrame.Dir", source=src),
    Signal(np.array([b"\x01\x02\x03", bytes(range(1, 9)), b"\xAA"], dtype="S8"), t, name="CAN_DataFrame.DataBytes",
           source=src, encoding="latin-1"),
], acq_name="CAN", acq_source=src)
m.save(os.path.join(out, "asammdf_vlsd.mf4"), overwrite=True)
m.save(os.path.join(out, "asammdf_vlsd_dz.mf4"), overwrite=True, compression=2)
