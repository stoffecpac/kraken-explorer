#!/bin/bash
# Setup physical CAN interfaces can0 and can1 for CAN FD
# Nominal bitrate 500 kbit/s, data bitrate 2 Mbit/s by default
# Requires: iproute2, a SocketCAN-capable adapter (e.g. gs_usb, peak_usb)
# Usage: ./setup_can.sh [bitrate] [dbitrate]   (default 500000 2000000)
#        Re-runs itself under sudo when not started as root.

set -e

. "$(dirname "$(readlink -f "$0")")/require_root.sh" # ip link / modprobe need CAP_NET_ADMIN

BITRATE="${1:-500000}"
DBITRATE="${2:-2000000}"

for dev in can0 can1; do
    if ! ip link show "$dev" > /dev/null 2>&1; then
        echo "Error: interface $dev not found" >&2
        exit 1
    fi

    # Bitrates can only be changed while the interface is down
    ip link set down "$dev"

    if ip link set "$dev" type can bitrate "$BITRATE" dbitrate "$DBITRATE" fd on 2> /dev/null; then
        echo "$dev: CAN FD, ${BITRATE} bit/s nominal, ${DBITRATE} bit/s data"
    else
        echo "Note: $dev does not support CAN FD, falling back to Classical CAN"
        ip link set "$dev" type can bitrate "$BITRATE" fd off 2> /dev/null \
            || ip link set "$dev" type can bitrate "$BITRATE"
        echo "$dev: Classical CAN, ${BITRATE} bit/s"
    fi

    # Auto-recovery from bus-off; not every driver supports it (e.g. some USB adapters)
    ip link set "$dev" type can restart-ms 100 2> /dev/null \
        || echo "Note: $dev does not support restart from bus-off, skipping restart-ms"

    ip link set up "$dev"
done

echo
echo "Test: cansend can0 123#DEADBEEF     ->  candump can1"
echo "      cansend can0 123##1DEADBEEF   ->  candump can1   (CAN FD, with BRS)"
