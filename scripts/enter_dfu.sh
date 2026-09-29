#!/bin/bash
# Puts the Seed into DFU mode for flashing. If it is already in DFU mode, does
# nothing; otherwise sends 'B' to the running firmware, which reboots into the
# STM32 ROM bootloader. Falls back to asking for BOOT + RESET.
set -euo pipefail

in_dfu() { dfu-util -l 2>/dev/null | grep -q '0483:df11'; }

in_dfu && exit 0

port=${1:-}
if [[ -z "$port" ]]; then
    shopt -s nullglob
    ports=(/dev/cu.usbmodem*)
    [[ ${#ports[@]} -eq 1 ]] && port=${ports[0]}
fi

if [[ -n "$port" && -c "$port" ]]; then
    echo "Asking firmware on $port to enter DFU mode..."
    printf B >"$port"
    for _ in $(seq 50); do
        in_dfu && exit 0
        sleep 0.1
    done
fi

echo 'Seed is not in DFU mode. Hold BOOT, tap RESET, release BOOT, then retry.' >&2
exit 1
