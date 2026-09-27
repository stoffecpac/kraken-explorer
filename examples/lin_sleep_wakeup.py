import kraken
import time

# Find the first LIN interface
lin_iface = next((i for i in kraken.interfaces() if i["bus_type"] == "LIN"), None)
if lin_iface is None:
    kraken.log_error("No LIN interface found")
else:
    lin_id = lin_iface["id"]
    kraken.log(f"Using LIN interface: {lin_iface['name']} (id={lin_id})")
    kraken.log(f"Initial state: {kraken.interface_state(lin_id)}")

    # Send go-to-sleep
    kraken.lin_sleep(lin_id)
    kraken.log("Sleep command sent")

    # Wait for the sleep frame echo on the bus
    deadline = time.time() + 2.0
    while time.time() < deadline:
        for msg in kraken.receive(timeout=0.1):
            if msg.is_lin_sleep:
                kraken.log("Sleep frame confirmed on bus")

    time.sleep(1)

    # Send wakeup
    kraken.lin_wakeup(lin_id)
    kraken.log("Wakeup command sent")

    # Wait for wakeup pulse echo
    deadline = time.time() + 2.0
    while time.time() < deadline:
        for msg in kraken.receive(timeout=0.1):
            if msg.is_lin_wakeup:
                kraken.log("Wakeup pulse confirmed on bus")
                break

    kraken.log(f"Final state: {kraken.interface_state(lin_id)}")
