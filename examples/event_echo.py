"""
Event-based message handler.

Waits (without polling) for incoming CAN messages. Each time a message
arrives, the on_message callback is invoked.  For every received message
a response with ID 0x123 and random data is sent back.

Usage: Run while a measurement is active.  Press Stop to end.
"""
import kraken
import os

# ---- configuration ----
RESPONSE_ID = 0x123
RESPONSE_DLC = 8
IFACE_NAME = "vcan0"         # TX interface; first interface if absent

def find_iface(name):
    """Id of the interface called `name`, else the first one in the measurement."""
    ifaces = kraken.interfaces()
    if not ifaces:
        raise RuntimeError("No interfaces in the measurement: add one in Setup and start the measurement")
    return next((i["id"] for i in ifaces if i["name"] == name), ifaces[0]["id"])

# Print available interfaces
for iface in kraken.interfaces():
    print(f"Interface {iface['id']}: {iface['name']}")

INTERFACE_ID = find_iface(IFACE_NAME)
print(f"Sending on interface {INTERFACE_ID}")

def on_message(msg):
    """Called for every received CAN message."""
    data_hex = msg.get_data().hex(' ')
    print(f"RX  0x{msg.id:03X}  [{msg.dlc}]  {data_hex}")

    # Skip sending a response for our own response ID to avoid loops
    if msg.id == RESPONSE_ID:
        return

    # Build a response with random payload
    resp = kraken.Message()
    resp.id = RESPONSE_ID
    resp.set_data(os.urandom(RESPONSE_DLC))
    kraken.send(resp, interface_id=INTERFACE_ID)

    resp_hex = resp.get_data().hex(' ')
    print(f"TX  0x{resp.id:03X}  [{resp.dlc}]  {resp_hex}")


# ---- event loop ----
print("Waiting for messages (event-based)...\n")

while True:
    # receive() blocks until at least one message arrives or timeout expires.
    # No CPU is consumed while waiting (uses a condition variable internally).
    messages = kraken.receive(timeout=1.0)

    for msg in messages:
        on_message(msg)
