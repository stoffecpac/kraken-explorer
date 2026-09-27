"""
Send CAN messages on a given interface.

Usage: Paste into the Script window and click Run while a measurement is active.
Sends on the interface named IFACE_NAME, else the first one in the measurement.
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

# Print available interfaces
for iface in kraken.interfaces():
    print(f"Interface {iface['id']}: {iface['name']}")

INTERFACE_ID = find_iface(IFACE_NAME)
print(f"Sending on interface {INTERFACE_ID}")

# --- Send a single standard CAN message ---
msg = kraken.Message()
msg.id = 0x123
msg.set_data(bytes([0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08]))
kraken.send(msg, interface_id=INTERFACE_ID)
print(f"Sent: ID=0x{msg.id:03X} DLC={msg.dlc} data={msg.get_data().hex(' ')}")

# --- Send an extended frame ---
msg2 = kraken.Message()
msg2.id = 0x18FEF100
msg2.extended = True
msg2.set_data(bytes([0xAA, 0xBB, 0xCC, 0xDD]))
kraken.send(msg2, interface_id=INTERFACE_ID)
print(f"Sent: ID=0x{msg2.id:08X} (ext) DLC={msg2.dlc} data={msg2.get_data().hex(' ')}")

# --- Send an RTR frame ---
msg3 = kraken.Message()
msg3.id = 0x200
msg3.rtr = True
msg3.dlc = 8
kraken.send(msg3, interface_id=INTERFACE_ID)
print(f"Sent: ID=0x{msg3.id:03X} RTR DLC={msg3.dlc}")

# --- Send periodic messages ---
print("\nSending 0x100 every 100ms (10 times)...")
periodic = kraken.Message()
periodic.id = 0x100
counter = 0

for i in range(10):
    periodic.set_data(bytes([counter & 0xFF, (counter >> 8) & 0xFF, 0, 0, 0, 0, 0, 0]))
    kraken.send(periodic, interface_id=INTERFACE_ID)
    print(f"  [{i+1}/10] counter={counter}")
    counter += 1
    time.sleep(0.1)

print("Done.")
