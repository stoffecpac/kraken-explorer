import cangaroo
import time

# Find the first LIN interface
lin_iface = next((i for i in cangaroo.interfaces() if i["bus_type"] == "LIN"), None)
if lin_iface is None:
    cangaroo.log_error("No LIN interface found")
else:
    lin_id = lin_iface["id"]
    cangaroo.log(f"Using LIN interface: {lin_iface['name']} (id={lin_id})")
    cangaroo.log(f"Initial state: {cangaroo.interface_state(lin_id)}")

    # Send go-to-sleep
    cangaroo.lin_sleep(lin_id)
    cangaroo.log("Sleep command sent")

    # Wait for the sleep frame echo on the bus
    deadline = time.time() + 2.0
    while time.time() < deadline:
        for msg in cangaroo.receive(timeout=0.1):
            if msg.is_lin_sleep:
                cangaroo.log("Sleep frame confirmed on bus")

    time.sleep(1)

    # Send wakeup
    cangaroo.lin_wakeup(lin_id)
    cangaroo.log("Wakeup command sent")

    # Wait for wakeup pulse echo
    deadline = time.time() + 2.0
    while time.time() < deadline:
        for msg in cangaroo.receive(timeout=0.1):
            if msg.is_lin_wakeup:
                cangaroo.log("Wakeup pulse confirmed on bus")
                break

    cangaroo.log(f"Final state: {cangaroo.interface_state(lin_id)}")
