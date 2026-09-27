"""
Save the current trace to files in every supported format.

cangaroo.save_trace(path, format=None) writes the trace buffer using the same
writers as File -> Save Trace. The format comes from the `format` argument, or is
inferred from the file extension when `format` is omitted.

Supported formats: 'candump', 'asc', 'mdf', 'pcap', 'pcapng'
(aliases: 'log' -> candump, 'mf4'/'mdf4' -> mdf, 'vector_asc' -> asc)

Unlike the GUI save dialog, an unrecognised extension is an error rather than a
silent fallback to ASC -- a script writing the wrong format is worse than one
that stops.

Usage: Paste into the Script window and click Run. Works while a measurement is
running (the writers take the trace lock) or after it has stopped.
"""
import os
import time

import cangaroo

OUT_DIR = "/tmp/cangaroo-traces"

os.makedirs(OUT_DIR, exist_ok=True)

n = cangaroo.trace_size()
print(f"trace holds {n} messages")

if n == 0:
    print("nothing to save -- start a measurement first")
else:
    stamp = time.strftime("%Y%m%d-%H%M%S")

    # Format inferred from the extension.
    for ext in ("asc", "candump", "mf4", "pcap", "pcapng"):
        path = os.path.join(OUT_DIR, f"trace-{stamp}.{ext}")
        cangaroo.save_trace(path)
        print(f"wrote {path} ({os.path.getsize(path)} bytes)")

    # Or state the format explicitly, whatever the file is called.
    explicit = os.path.join(OUT_DIR, f"trace-{stamp}.bin")
    cangaroo.save_trace(explicit, format="pcapng")
    print(f"wrote {explicit} as pcapng ({os.path.getsize(explicit)} bytes)")

    # Errors are raised, not returned.
    try:
        cangaroo.save_trace(os.path.join(OUT_DIR, "trace.txt"))
    except ValueError as e:
        print(f"expected failure: {e}")

    try:
        cangaroo.save_trace("/nonexistent-dir/trace.asc")
    except RuntimeError as e:
        print(f"expected failure: {e}")
