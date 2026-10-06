#pragma once

#include <cstddef>

// The carrier board's volume and tone knobs, applied to the final output.
// Volume follows an audio taper down to silence. Tone is a 12 dB/octave
// low-pass (two one-pole stages; a guitar's own tone control is one) whose
// corner sweeps from 400 Hz up to fully open, where the signal passes
// through unchanged.
//
// Both pots are linear 10k dividers from 3V3_A into the Daisy's ADC, so
// the readings are fractions of full travel. Knob moves glide with a 20 ms
// time constant, which keeps a turned knob or a noisy reading from clicking.
class OutputControls {
  public:
    // The tone filter is designed at compile time for this rate only.
    static constexpr double kSampleRate = 48000.0;

    // Sets the knob readings, each in [0, 1]. Cheap enough to call every
    // block; Process glides toward the latest ones.
    void SetKnobs(float volume, float tone);

    // Jumps to the latest readings without gliding and clears the filter's
    // memory, so the next sample starts from silence at the knobs' settings.
    void Reset();

    // Filters and scales samples in place. Never allocates. The volume ramps
    // sample by sample within a block; the tone corner moves once per block.
    void Process(float *samples, std::size_t frame_count);

  private:
    // Knob positions from the latest readings, and where the glide has got to.
    float volume_target_ = 1.0f;
    float tone_target_ = 1.0f;
    float volume_ = 1.0f;
    float tone_ = 1.0f;

    // Gain at the end of the last block, where the next block's ramp starts.
    float gain_ = 1.0f;

    // The tone filter's two stage outputs from the last sample.
    float stage1_ = 0.0f;
    float stage2_ = 0.0f;
};
