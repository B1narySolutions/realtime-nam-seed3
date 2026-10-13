#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
daisy_commit=cc146d5065dd8286078a662e2830bf820c37a612
nam_commit=2563c0fd4cb1f9ce457d89a761738ea15097e1f3

# Keep already-installed versions; upgrading tools is a separate decision.
case $(uname -s) in
    Darwin)
        command -v brew >/dev/null || {
            echo 'Install Homebrew first: https://brew.sh' >&2
            exit 1
        }
        if ! brew list --cask gcc-arm-embedded >/dev/null 2>&1; then
            brew install --cask gcc-arm-embedded
        fi
        for formula in dfu-util; do
            if ! brew list --formula "$formula" >/dev/null 2>&1; then
                brew install "$formula"
            fi
        done
        ;;
    Linux)
        sudo_command=()
        if (( EUID != 0 )); then
            command -v sudo >/dev/null || {
                echo 'Install sudo or run this script as root.' >&2
                exit 1
            }
            sudo_command=(sudo)
        fi
        if command -v apt-get >/dev/null; then
            "${sudo_command[@]}" apt-get update
            "${sudo_command[@]}" apt-get install -y --no-upgrade \
                build-essential git gcc-arm-none-eabi libnewlib-arm-none-eabi \
                libstdc++-arm-none-eabi-newlib dfu-util curl ca-certificates \
                libdigest-sha-perl python3 python3-venv screen
        elif command -v pacman >/dev/null; then
            # No -y: refreshing the package list without upgrading is a partial
            # upgrade, which Arch doesn't support. Run pacman -Syu first if this
            # can't find a package.
            "${sudo_command[@]}" pacman -S --needed --noconfirm \
                base-devel git arm-none-eabi-gcc arm-none-eabi-newlib dfu-util \
                curl ca-certificates perl python screen
            # Arch installs shasum outside the default PATH until the next login.
            [[ -x /usr/bin/core_perl/shasum ]] && PATH="$PATH:/usr/bin/core_perl"
        else
            echo 'Linux setup requires apt (Ubuntu/Debian) or pacman (Arch).' >&2
            exit 1
        fi

        # Let the logged-in user reach the Seed's DFU bootloader and USB serial
        # port without root or extra groups.
        rules=/etc/udev/rules.d/50-daisy-seed.rules
        if [[ ! -f "$rules" ]]; then
            printf '%s\n' \
                'SUBSYSTEMS=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="df11", MODE="0664", TAG+="uaccess"' \
                'SUBSYSTEMS=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="5740", MODE="0664", TAG+="uaccess"' |
                "${sudo_command[@]}" tee "$rules" >/dev/null
            "${sudo_command[@]}" udevadm control --reload-rules
            "${sudo_command[@]}" udevadm trigger
        fi
        ;;
    *)
        echo 'Supported platforms: macOS (Homebrew), Ubuntu/Debian (apt) and Arch (pacman).' >&2
        exit 1
        ;;
esac
for tool in git make arm-none-eabi-g++ dfu-util; do
    command -v "$tool" >/dev/null || {
        echo "Missing $tool on PATH; check your tool installation and shell setup." >&2
        exit 1
    }
done
mkdir -p "$root/libs"

if [[ ! -d "$root/libs/libDaisy" ]]; then
    git clone --no-checkout https://github.com/electro-smith/libDaisy.git "$root/libs/libDaisy"
    git -C "$root/libs/libDaisy" checkout --detach "$daisy_commit"
fi
if [[ $(git -C "$root/libs/libDaisy" rev-parse HEAD) != "$daisy_commit" ]]; then
    echo "libs/libDaisy differs from pinned revision $daisy_commit; leaving it untouched." >&2
    exit 1
fi
git -C "$root/libs/libDaisy" submodule update --init --recursive

if [[ ! -d "$root/libs/NeuralAmpModelerCore" ]]; then
    git clone --no-checkout https://github.com/sdatkinson/NeuralAmpModelerCore.git "$root/libs/NeuralAmpModelerCore"
    git -C "$root/libs/NeuralAmpModelerCore" checkout --detach "$nam_commit"
fi
if [[ $(git -C "$root/libs/NeuralAmpModelerCore" rev-parse HEAD) != "$nam_commit" ]]; then
    echo "libs/NeuralAmpModelerCore differs from pinned revision $nam_commit; leaving it untouched." >&2
    exit 1
fi
git -C "$root/libs/NeuralAmpModelerCore" submodule update --init --recursive

arm-none-eabi-g++ --version
dfu-util --version
echo 'Dependencies ready. Run make download-models, then make build.'
