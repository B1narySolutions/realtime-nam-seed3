#!/bin/bash
set -euo pipefail

command -v screen >/dev/null || {
    echo 'Missing screen; install it before running make monitor.' >&2
    exit 1
}

port=${1:-}
if [[ -z "$port" ]]; then
    shopt -s nullglob
    ports=(/dev/cu.usbmodem* /dev/ttyACM*)
    case ${#ports[@]} in
        0)
            echo 'No USB serial port found. Connect the Seed3 and reset into the application (not DFU mode).' >&2
            exit 1
            ;;
        1) port=${ports[0]} ;;
        *)
            echo 'Multiple USB serial ports found. Choose one with make monitor PORT=/dev/cu.usbmodem... (or /dev/ttyACM... on Linux)' >&2
            printf '  %s\n' "${ports[@]}" >&2
            exit 1
            ;;
    esac
fi

[[ -c "$port" ]] || { echo "Not a serial device: $port" >&2; exit 1; }
[[ -t 0 && -t 1 ]] || { echo 'Run make monitor in an interactive terminal.' >&2; exit 1; }
echo "Opening $port at 115200. Exit with Ctrl-A, then K, then Y."
exec screen "$port" 115200
