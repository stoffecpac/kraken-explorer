"""
Send periodic CAN messages using send_periodic() / stop_periodic().

Each periodic task runs on its own background thread inside Kraken Explorer, so the
script can wait for incoming messages at the same time without any manual
timing loop.  All tasks are automatically stopped when the script stops.

Demonstrates:
  - kraken.send_periodic()  — start a repeating TX task, returns a handle
  - kraken.stop_periodic()  — cancel one task by handle
  - kraken.receive()        — wait for replies while periodic TX is running

Usage: Load demo.dbc in Measurement Setup, start the measurement, then run.
Sends on the interface named IFACE_NAME, else the first one in the
measurement; adjust the intervals to match your setup.
"""
import kraken
import time

IFACE_NAME = "vcan0"         # TX interface; first interface if absent

def find_iface(name):
    """Id of the interface called `name`, else the first one in the measurement."""
    ifaces = kraken.interfaces()
    if not ifaces:
        raise RuntimeError("No interfaces in the measurement: add one in Setup and start the measurement")
    return next((i["id"] for i in ifaces if i["name"] == name), ifaces[0]["id"])

# ---- show available interfaces ----
for iface in kraken.interfaces():
    print(f"Interface {iface['id']}: {iface['name']}")

INTERFACE_ID = find_iface(IFACE_NAME)
print(f"Sending on interface {INTERFACE_ID}")

# ---- build messages to send periodically ----
heartbeat = kraken.Message()
heartbeat.id = 0x700
heartbeat.set_data(bytes([0x01]))

# Use encode() so we get the right DBC bit layout
engine_msg = kraken.encode("EngineData", {
    "EngineSpeed": 2000.0,
    "EngineTemp":  85.0,
    "OilPressure": 3.8,
})

# ---- start periodic tasks ----
h_heartbeat = kraken.send_periodic(heartbeat,   interval_ms=100,  interface_id=INTERFACE_ID)
h_engine    = kraken.send_periodic(engine_msg,  interval_ms=20,   interface_id=INTERFACE_ID)

print(f"Started heartbeat  (handle={h_heartbeat}, every 100 ms)")
print(f"Started EngineData (handle={h_engine},    every 20 ms)")
print("Listening for incoming messages for 3 seconds...\n")

# ---- receive loop while periodic TX runs in the background ----
deadline = time.time() + 3.0
while time.time() < deadline:
    msgs = kraken.receive(timeout=0.1)
    for msg in msgs:
        decoded = kraken.decode(msg)
        if decoded:
            print(f"RX  0x{msg.id:03X}  {decoded['message']}")
        else:
            print(f"RX  0x{msg.id:03X}  [{msg.dlc}]  {msg.get_data().hex(' ')}")

# ---- stop individual tasks ----
kraken.stop_periodic(h_engine)
print(f"\nStopped EngineData (handle={h_engine}).")
print("Heartbeat still running for 1 more second...")
time.sleep(1.0)

# h_heartbeat is stopped automatically when the script exits,
# but we can also stop it explicitly:
kraken.stop_periodic(h_heartbeat)
print(f"Stopped heartbeat (handle={h_heartbeat}).")
print("Done.")
