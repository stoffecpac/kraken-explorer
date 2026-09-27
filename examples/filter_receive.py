"""
Receive only selected CAN IDs using set_filter() / clear_filter().

The filter is applied inside Kraken Explorer before messages enter the receive()
queue, so filtered-out traffic never reaches the script.

Demonstrates:
  - kraken.set_filter()    — accept only messages whose (id & mask) == id,
                               optionally only from one interface (interface_id=)
  - kraken.rx_dropped()    — frames lost because receive() fell behind
  - kraken.clear_filter()  — remove the filter, accept everything again
  - kraken.trace_size()    — total messages in the trace buffer
  - kraken.clear_trace()   — reset the trace buffer

Usage: Start the measurement, then run.  Works with or without a DBC loaded.
The per-interface filter uses the interface named IFACE_NAME, else the first
one in the measurement.
"""
import kraken
import time

IFACE_NAME = "vcan0"         # for the per-interface filter

def find_iface(name):
    """Id of the interface called `name`, else the first one in the measurement."""
    ifaces = kraken.interfaces()
    if not ifaces:
        raise RuntimeError("No interfaces in the measurement: add one in Setup and start the measurement")
    return next((i["id"] for i in ifaces if i["name"] == name), ifaces[0]["id"])

# ---- helper ----
def receive_for(seconds, label):
    """Drain the receive queue for `seconds`, printing each message."""
    print(f"\n--- {label} ---")
    deadline = time.time() + seconds
    count = 0
    while time.time() < deadline:
        for msg in kraken.receive(timeout=0.1):
            decoded = kraken.decode(msg)
            if decoded:
                sig_summary = ", ".join(
                    f"{n}={v['value']:.4g}{v['unit']}"
                    for n, v in decoded["signals"].items()
                )
                print(f"  0x{msg.id:03X}  {decoded['message']}  {sig_summary}")
            else:
                print(f"  0x{msg.id:03X}  [{msg.dlc}]  {msg.get_data().hex(' ')}")
            count += 1
    print(f"  ({count} message(s) received)")


# ---- baseline: no filter ----
kraken.clear_trace()
receive_for(1.0, "No filter — all traffic")
print(f"Trace size after 1 s: {kraken.trace_size()} messages")

# ---- filter: accept only EngineData (ID 0x100 = 256) exactly ----
kraken.set_filter(0x100, mask=0x7FF)
receive_for(1.0, "Filter: ID 0x100 only (EngineData)")

# ---- filter: accept 0x100–0x1FF range (top 4 bits match 0x100) ----
kraken.set_filter(0x100, mask=0x700)
receive_for(1.0, "Filter: 0x100–0x1FF range")

# ---- filter: accept only extended frames ----
kraken.set_filter(0x00000000, mask=0x00000000, extended=True)
receive_for(1.0, "Filter: extended frames only")

# ---- filter: all IDs, but only from one interface ----
INTERFACE_ID = find_iface(IFACE_NAME)
kraken.set_filter(0, mask=0, interface_id=INTERFACE_ID)
receive_for(1.0, f"Filter: interface {INTERFACE_ID} ({kraken.interface_name(INTERFACE_ID)}) only")

# ---- remove filter ----
kraken.clear_filter()
kraken.clear_trace()
receive_for(1.0, "Filter cleared — all traffic again")
print(f"Trace size after clear + 1 s: {kraken.trace_size()} messages")

print(f"Frames dropped (queue full): {kraken.rx_dropped()}")
print("\nDone.")
