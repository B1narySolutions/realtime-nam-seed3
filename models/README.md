# Amp models

`amps.json` lists the amps in `AmpId` order: each amp's name, model file,
download URL and SHA-256, and `includes_cabinet`, true when the capture
already has a mic'd speaker so the firmware skips its cabinet EQ. It is the
only place that list is written down; the download script, the converter, Make
and the tests all read it, and the firmware build fails if it disagrees with `AmpId` in `src/models/amp_models.h`.
To add an amp, add an entry here and a matching `AmpId` value.

`input_trim_db` and `output_trim_db` level each amp: the firmware scales the
model's input and output by them, and bypass skips both. Each must be within
±24 dB. The converter turns them into linear gains so the firmware needn't
link `pow`.

- **Input trim** makes the model see about the level its capture was made at.
  The Twin's metadata records 0 dBFS as +18.2 dBu (`input_level_dbu`). The rev 2
  carrier board's unity-gain guitar input reaches the Daisy's full scale at
  about 3.9 Vpp, or +5.0 dBu, at the jack (`audio-board/rev2/DESIGN.md` in the
  carrier board repository). So the Twin's trim is 5.0 − 18.2 = −13.2 dB. On
  rev 1, the analog input level knob (RV1) comes first, so the trim is exact
  only where that knob gives unity gain. The other three captures don't record
  their level, so their trims are 0 dB.
- **Output trim** plays every amp at −28 LUFS, about the four amps' average, so
  switching amps no longer jumps by up to 12 LU. Each amp's loudness was
  measured through the firmware's audio path, with its input trim, the cabinet
  EQ on, the knobs fully up and `kOutputGain` applied. The input was the guitar
  section of `reamp_signal.wav` (44–150 s), scaled to peak at −12 dBFS, about a
  hard strum on passive pickups. Their relative loudness moved by under 1.5 LU
  with the input anywhere from −12 to 0 dBFS peak.

`local/` holds the downloaded A2-Lite models used by the firmware.

Run `make download-models` from the repository root to obtain them.
These files are Git-ignored. Their T3K licenses permit local use but require
author permission to redistribute the models or firmware containing their weights.

`scripts/convert_a2.py` validates the supported 48 kHz architecture and packs
the models into `build/generated/embedded_a2_data.h`. The generated header also
checks at compile time that `src/models/a2_lite.h` has the shape it validated. Make regenerates
that header when an input model or the converter changes, or the header is missing.
