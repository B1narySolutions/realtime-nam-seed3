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
    static constexpr float kInputGain = 1.0f;
    static constexpr float kOutputGain = 0.8f;
    static_assert(CabFilter::kSampleRate == kSampleRate, "The cabinet is designed for one sample rate");
    static_assert(OutputControls::kSampleRate == kSampleRate, "The tone filter is designed for one sample rate");

    // Call with audio stopped. Failure leaves the path in bypass.
    bool LoadAmpModel(AmpId amp = AmpId::Fender);
    // Call with audio stopped. Loads an A2-Lite model uploaded from the touchscreen (weights in
    // the engine's packed order, as LoadAmpModel's embedded ones); the weights must stay valid
    // while it runs. The cabinet EQ applies. Failure leaves the path in bypass.
    bool LoadUploadedModel(const float *weights);

    // Call with audio stopped. Turns the cabinet EQ after the model on or off
    // (it starts on) and clears its memory either way. The setting survives
    // model switches, but an amp whose capture includes a cabinet always
    // skips the EQ (see CabinetApplies).
    void SetCabinet(bool enabled);
    bool CabinetEnabled() const { return cabinet_enabled_; }
    // Whether the loaded amp's capture leaves the cabinet EQ to this path.
    bool CabinetApplies() const { return !amp_includes_cabinet_; }

    // The volume and tone knob readings, each in [0, 1]. Process glides
    // toward the latest ones (see OutputControls); they start fully up.
    void SetKnobs(float volume, float tone) { controls_.SetKnobs(volume, tone); }

    // Call with audio stopped. Jumps to the latest knob readings without
    // gliding, so audio starts at the knobs' settings rather than gliding
    // down from full volume.
    void ResetKnobs() { controls_.Reset(); }

    // Left input to both outputs. Bypass skips the model and the cabinet
    // entirely, so their state stays where bypass began. The volume and
    // tone knobs apply either way. Buffers must contain at least
    // frame_count samples.
    void Process(const float *input_left, float *output_left, float *output_right, std::size_t frame_count, bool bypass_model);

  private:
    bool ProcessModel(const float *input, std::size_t frame_count);

    NamProcessor processor_;
    bool ready_ = false;
    CabFilter cabinet_;
    bool cabinet_enabled_ = true;
    bool amp_includes_cabinet_ = false;
    OutputControls controls_;
    // Scratch buffers so the model never reads or writes the caller's buffers.
    std::array<float, kBlockSize> input_{};
    std::array<float, kBlockSize> output_{};
};
