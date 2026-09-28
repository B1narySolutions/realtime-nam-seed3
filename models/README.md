# Amp models

`amps.json` lists the amps in `AmpId` order: each amp's name, model file,
download URL and SHA-256. It is the only place that list is written down; the
download script, the converter, Make and the tests all read it, and the
firmware build fails if it disagrees with `AmpId` in `src/models/amp_models.h`.
To add an amp, add an entry here and a matching `AmpId` value.

`local/` holds the downloaded A2-Lite models used by the firmware.

Run `bash scripts/download_models.sh` from the repository root to obtain them.
These files are Git-ignored. Their T3K licenses permit local use but require
author permission to redistribute the models or firmware containing their weights.

`scripts/convert_a2.py` validates the supported 48 kHz architecture and packs
the models into `build/generated/embedded_a2_data.h`. The generated header also
checks at compile time that `src/models/a2_lite.h` has the shape it validated. Make regenerates
that header when an input model or the converter changes, or the header is missing.
