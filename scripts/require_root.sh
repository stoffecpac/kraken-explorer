#!/bin/bash
# Sourced by setup_can.sh / setup_vcan.sh: re-runs the calling script under sudo when not
# root, rather than failing partway through and leaving half the setup in place.
if [[ $EUID -ne 0 ]]; then
    if ! command -v sudo > /dev/null 2>&1; then
        echo "Error: root privileges are required and sudo was not found." >&2
        echo "       Run this script as root instead." >&2
        exit 1
    fi
    echo "Root privileges required, re-running under sudo..." >&2
    exec sudo -- "$(readlink -f "$0")" "$@"
fi
