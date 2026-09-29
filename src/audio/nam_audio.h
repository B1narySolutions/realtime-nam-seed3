#pragma once

#include "models/amp_models.h"
#include "audio/nam_processor.h"
#include <array>
#include <cstddef>

class NamAudio {
  public:
    static constexpr std::size_t kBlockSize = 48;
    static constexpr double kSampleRate = 48000.0;
    static constexpr float kInputGain = 1.0f;
    static constexpr float kOutputGain = 0.8f;

    // Call with audio stopped. Failure leaves the path in bypass.
    bool LoadAmpModel(AmpId amp = AmpId::Fender);

    // Left input to both outputs. Bypass skips the model entirely, so its
    // state stays where bypass began. Buffers must contain at least
    // frame_count samples.
    void Process(const float *input_left, float *output_left, float *output_right, std::size_t frame_count, bool bypass_model);

  private:
    bool ProcessModel(const float *input, std::size_t frame_count);

    NamProcessor processor_;
    bool ready_ = false;
    // Scratch buffers so the model never reads or writes the caller's buffers.
    std::array<float, kBlockSize> input_{};
    std::array<float, kBlockSize> output_{};
};
