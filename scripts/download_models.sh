#!/bin/bash
# Download the amp captures listed in models/amps.json and extract their
# existing A2-Lite models. convert_a2.py does the extraction and validation.
# T3K: local use permitted; redistribution requires author permission.
set -euo pipefail

cd "$(dirname "$0")/.."
for tool in curl shasum python3; do
    command -v "$tool" >/dev/null || { echo "Missing dependency: $tool" >&2; exit 1; }
done

sources=$(python3 scripts/convert_a2.py sources)
output_dir="$PWD/models/local"
mkdir -p "$output_dir"
staging=$(mktemp -d "$output_dir/.download.XXXXXX")
trap 'rm -rf "$staging"' EXIT

names=()
while IFS=$'\t' read -r name url expected_sha; do
    echo "Downloading $name..."
    curl --fail --silent --show-error --location \
        --connect-timeout 15 --max-time 60 \
        "$url" -o "$staging/source.nam"
    actual_sha=$(shasum -a 256 "$staging/source.nam")
    if [[ ${actual_sha%% *} != "$expected_sha" ]]; then
        echo "$name: upstream checksum changed" >&2
        exit 1
    fi
    python3 scripts/convert_a2.py extract "$staging/source.nam" "$staging/$name"
    names+=("$name")
done <<< "$sources"

# Replace existing models only after all downloads and extraction succeed.
for name in "${names[@]}"; do
    mv "$staging/$name" "$output_dir/$name"
    echo "Saved $output_dir/$name"
done
