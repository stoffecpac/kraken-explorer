"""
LIN bus demonstration.

Demonstrates:
  - kraken.lin_databases()    — list loaded LDF files and their frames
  - kraken.find_lin_frame()   — look up a LIN frame definition by name or ID
  - kraken.make_lin_message() — build a LIN BusMessage
  - kraken.send()             — transmit the frame on a LIN interface
  - kraken.decode_lin()       — decode a received LIN frame using an LDF
  - msg.bustype                 — distinguish LIN from CAN in a mixed trace

Usage:
  1. Add your LDF file in Measurement Setup and connect a LIN-capable interface
     (e.g. a GrIP LIN channel).
  2. Start the measurement.
  3. Paste this script into the Script window and click Run.
  4. Adjust IFACE_NAME, FRAME_NAME, and signal values to match your LDF.

Sends on the LIN interface named IFACE_NAME, else the first LIN interface in
the measurement.  Without a LIN interface the send step is skipped.
"""
import kraken
import time

IFACE_NAME   = ""               # LIN interface name; first LIN interface if absent
FRAME_NAME   = "MasterToSlave1"  # from demo.ldf
LISTEN_SECS  = 5.0              # how long to listen for incoming LIN frames

# ---------------------------------------------------------------------------
# 1. Show available interfaces
# ---------------------------------------------------------------------------
def find_lin_iface(name):
    """Id of the LIN interface called `name`, else the first LIN one, else None."""
    lin = [i for i in kraken.interfaces() if i["bus_type"] == "LIN"]
    return next((i["id"] for i in lin if i["name"] == name), lin[0]["id"] if lin else None)

print("=== Interfaces ===")
for iface in kraken.interfaces():
    print(f"  [{iface['id']}] {iface['name']}  ({iface['bus_type']})")
INTERFACE_ID = find_lin_iface(IFACE_NAME)
print()

# ---------------------------------------------------------------------------
# 2. Show loaded LDF databases
# ---------------------------------------------------------------------------
print("=== LDF Databases ===")
ldfs = kraken.lin_databases()
if not ldfs:
    print("  No LDF files loaded.  Add one in Measurement Setup.")
else:
    for db in ldfs:
        print(f"  {db['file']}  (network: {db['network']}, speed: {db['speed']} bps)")
        print(f"  Master: {db['master']}")
        for frame in db['frames']:
            sig_names = ", ".join(s['name'] for s in frame['signals'])
            print(f"    ID=0x{frame['id']:02X}  {frame['name']!r}"
                  f"  [{frame['length']} B]  signals: {sig_names or '(none)'}")
print()

# ---------------------------------------------------------------------------
# 3. Inspect a specific frame definition
# ---------------------------------------------------------------------------
print(f"=== Frame definition: {FRAME_NAME!r} ===")
frame_def = kraken.find_lin_frame(FRAME_NAME)
if frame_def is None:
    print(f"  Frame {FRAME_NAME!r} not found — check your LDF and FRAME_NAME.")
else:
    print(f"  ID=0x{frame_def['id']:02X}  length={frame_def['length']} B"
          f"  publisher={frame_def['publisher']!r}")
    for sig in frame_def['signals']:
        print(f"    {sig['name']:20s}  bit {sig['bit_offset']}+{sig['bit_length']}"
              f"  factor={sig['factor']}  offset={sig['offset']}"
              f"  unit={sig['unit']!r}")
print()

# Also look up by numeric ID (0x01 as an example)
frame_by_id = kraken.find_lin_frame(0x01)
if frame_by_id:
    print(f"Frame at ID 0x01: {frame_by_id['name']!r}")
print()

# ---------------------------------------------------------------------------
# 4. Build and send a LIN frame
# ---------------------------------------------------------------------------
print("=== Send LIN frame ===")
if INTERFACE_ID is None:
    print("  No LIN interface in the measurement: add a LIN-capable interface in Setup.")
else:
    msg = kraken.make_lin_message(0x01, 4)
    msg.set_data(bytes([0x11, 0x22, 0x33, 0x44]))
    kraken.send(msg, interface_id=INTERFACE_ID)
    print(f"  Sent: {msg}")

    # Send a second frame using a known frame ID from the LDF
    if frame_def is not None:
        lin_id = frame_def['id']
        length  = frame_def['length']
        tx = kraken.make_lin_message(lin_id, length)
        # Fill with a simple incrementing pattern
        tx.set_data(bytes(i & 0xFF for i in range(length)))
        kraken.send(tx, interface_id=INTERFACE_ID)
        print(f"  Sent {FRAME_NAME!r}: {tx}")

        # Verify decode of the just-sent frame
        decoded = kraken.decode_lin(tx)
        if decoded:
            print(f"  Decoded {decoded['frame']!r}  ID=0x{decoded['id']:02X}")
            for name, sig in decoded['signals'].items():
                label = sig.get('value_name') or f"{sig['value']:.4g} {sig['unit']}"
                print(f"    {name}: {label}  [raw={sig['raw']}]")
print()

# ---------------------------------------------------------------------------
# 5. Receive and decode incoming frames (LIN + CAN mixed)
# ---------------------------------------------------------------------------
print(f"=== Listening for {LISTEN_SECS:.0f} s (LIN and CAN) ===")
deadline = time.time() + LISTEN_SECS
lin_count = 0
can_count = 0

while time.time() < deadline:
    for msg in kraken.receive(timeout=0.2):
        if msg.bustype == "LIN":
            lin_count += 1
            decoded = kraken.decode_lin(msg)
            if decoded:
                sig_str = "  ".join(
                    f"{n}={v.get('value_name') or format(v['value'], '.4g') + v['unit']}"
                    for n, v in decoded['signals'].items()
                )
                direction = "RX" if msg.is_rx else "TX"
                print(f"  LIN {direction}  ID=0x{msg.id:02X}  {decoded['frame']!r}"
                      f"  [{msg.dlc} B]  {sig_str}")
            else:
                direction = "RX" if msg.is_rx else "TX"
                print(f"  LIN {direction}  ID=0x{msg.id:02X}"
                      f"  [{msg.dlc} B]  {msg.get_data().hex(' ')}  (no LDF definition)")
        else:
            can_count += 1
            decoded = kraken.decode(msg)
            if decoded:
                print(f"  CAN  0x{msg.id:03X}  {decoded['message']!r}")
            else:
                print(f"  CAN  0x{msg.id:03X}  [{msg.dlc}]  {msg.get_data().hex(' ')}")

print(f"\nReceived {lin_count} LIN frame(s) and {can_count} CAN frame(s).")
print("Done.")
