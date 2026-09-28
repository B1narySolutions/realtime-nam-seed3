#!/bin/bash
# Download the amp captures listed in models/amps.json and extract their
# existing A2-Lite models.
# T3K: local use permitted; redistribution requires author permission.
set -euo pipefail

cd "$(dirname "$0")/.."
for tool in curl jq shasum; do
    command -v "$tool" >/dev/null || { echo "Missing dependency: $tool" >&2; exit 1; }
done

output_dir="$PWD/models/local"
mkdir -p "$output_dir"
staging=$(mktemp -d "$output_dir/.download.XXXXXX")
trap 'rm -rf "$staging"' EXIT

download_lite() {
    local name=$1 url=$2 expected_sha=$3 actual_sha
    echo "Downloading $name..."
    curl --fail --silent --show-error --location \
        --connect-timeout 15 --max-time 60 \
        "$url" -o "$staging/source.nam"
    actual_sha=$(shasum -a 256 "$staging/source.nam")
    if [[ ${actual_sha%% *} != "$expected_sha" ]]; then
        echo "$name: upstream checksum changed" >&2
        exit 1
    fi

    # Preserve Lite weights/configuration and inherit container calibration.
    jq -ce '
        . as $container
        | if .architecture != "SlimmableContainer" then
            error("expected an A2 SlimmableContainer")
          else . end
        | [.config.submodels[] | select(.max_value == 0.5) | .model]
        | if length != 1 then error("expected one Lite submodel") else .[0] end
        | .sample_rate = (.sample_rate // $container.sample_rate)
        | .metadata = (($container.metadata // {}) + (.metadata // {}))
        | if .architecture == "WaveNet" and .sample_rate == 48000
             and (.weights | length) == 1871
          then . else error("unexpected Lite model format") end
    ' "$staging/source.nam" > "$staging/$name"
}

names=()
while IFS=$'\t' read -r name url expected_sha; do
    download_lite "$name" "$url" "$expected_sha"
    names+=("$name")
done < <(jq -r '.[] | [.file, .url, .download_sha256] | @tsv' models/amps.json)

# Replace existing models only after all downloads and extraction succeed.
for name in "${names[@]}"; do
    mv "$staging/$name" "$output_dir/$name"
    echo "Saved $output_dir/$name"
done
