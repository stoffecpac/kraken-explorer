"""
Start / stop measurements and inspect the trace from a script.

Demonstrates:
  - kraken.measurement_running()  — check whether a measurement is active
  - kraken.start_measurement()    — start the measurement programmatically
  - kraken.stop_measurement()     — stop the measurement programmatically
  - kraken.trace_size()           — number of messages currently in the trace
  - kraken.clear_trace()          — reset the trace buffer
  - kraken.find_message()         — inspect a DBC message definition by name

Usage: Configure your interfaces in Measurement Setup (no need to start the
measurement manually), then run this script.
"""
import kraken
import time

# ---- check initial state ----
if kraken.measurement_running():
    print("Measurement is already running — stopping it first.")
    kraken.stop_measurement()

print(f"Measurement running: {kraken.measurement_running()}")
print(f"Trace size before start: {kraken.trace_size()} messages")

# ---- start measurement ----
ok = kraken.start_measurement()
print(f"\nstart_measurement() -> {ok}")
print(f"Measurement running: {kraken.measurement_running()}")

# ---- collect traffic for a few seconds ----
print("\nCollecting traffic for 3 seconds...")
time.sleep(3.0)
print(f"Trace size after 3 s: {kraken.trace_size()} messages")

# ---- inspect DBC definitions (no live message needed) ----
for name in ("EngineData", "TransmissionData", "AmbientData"):
    defn = kraken.find_message(name)
    if defn is None:
        print(f"\n{name}: not found (is demo.dbc loaded?)")
    else:
        sig_names = [s["name"] for s in defn["signals"]]
        print(f"\n{defn['message']}  ID=0x{defn['id']:03X}  DLC={defn['dlc']}")
        print(f"  Signals: {', '.join(sig_names)}")

# ---- clear trace and collect again ----
print("\nClearing trace...")
kraken.clear_trace()
print(f"Trace size after clear: {kraken.trace_size()} messages")

time.sleep(1.0)
print(f"Trace size after 1 more second: {kraken.trace_size()} messages")

# ---- stop measurement ----
ok = kraken.stop_measurement()
print(f"\nstop_measurement() -> {ok}")
print(f"Measurement running: {kraken.measurement_running()}")
print(f"Final trace size: {kraken.trace_size()} messages")

print("\nDone.")
