#pragma once

#include "audio/cab_filter.h"
#include "audio/nam_processor.h"
#include "audio/output_controls.h"
#include "models/amp_models.h"
#include <array>
#include <cstddef>

class NamAudio {
  public:
    static constexpr std::size_t kBlockSize = 48;
    static constexpr double kSampleRate = 48000.0;
    static constexpr float kOutputGain = 0.8f;
    static_assert(CabFilter::kSampleRate == kSampleRate, "The cabinet is designed for one sample rate");
    static_assert(OutputControls::kSampleRate == kSampleRate, "The tone filter is designed for one sample rate");

    // Call with audio stopped. Failure leaves the path in bypass.
    bool LoadAmpModel(AmpId amp = AmpId::Fender);

    // Call with audio stopped. Turns the cabinet EQ after the model on or off
    // and clears its memory either way. Each LoadAmpModel resets it to the
    // amp's default: on, unless the amp's capture includes its own cabinet.
    void SetCabinet(bool enabled);
    bool CabinetEnabled() const { return cabinet_enabled_; }

    // The volume and tone knob readings, each in [0, 1]. Process glides
    // toward the latest ones (see OutputControls); they start fully up.
    void SetKnobs(float volume, float tone) { controls_.SetKnobs(volume, tone); }

    // Call with audio stopped. Jumps to the latest knob readings without
    // gliding, so audio starts at the knobs' settings rather than gliding
    // down from full volume.
    void ResetKnobs() { controls_.Reset(); }

    // Left input to both outputs. The amp's input and output trims (see
    // AmpInputGain and AmpOutputGain) scale the model's input and output.
    // Bypass skips the model, its trims and the cabinet entirely, so the
    // model's and cabinet's state stays where bypass began. The volume and
    // tone knobs apply either way. Buffers must contain at least
    // frame_count samples.
    void Process(const float *input_left, float *output_left, float *output_right, std::size_t frame_count, bool bypass_model);

  private:
    bool ProcessModel(const float *input, std::size_t frame_count);

    NamProcessor processor_;
    bool ready_ = false;
    CabFilter cabinet_;
    bool cabinet_enabled_ = true;
    // The loaded amp's trims, as linear gains.
    float input_gain_ = 1.0f;
    float output_gain_ = 1.0f;
    OutputControls controls_;
    // Scratch buffers so the model never reads or writes the caller's buffers.
    std::array<float, kBlockSize> input_{};
    std::array<float, kBlockSize> output_{};
};
