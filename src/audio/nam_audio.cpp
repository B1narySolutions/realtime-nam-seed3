#include "audio/nam_audio.h"
#include <algorithm>
#include <cmath>

namespace {
// Scales a sample and limits it to [-1, 1]; NaN and infinity become silence.
float ApplyOutputGainAndClamp(float sample) {
    if (!std::isfinite(sample))
        return 0.0f;
    return std::max(-1.0f, std::min(1.0f, sample * NamAudio::kOutputGain));
}
} // namespace

bool NamAudio::LoadAmpModel(AmpId amp) {
    ready_ = false;

    // Free the old model first; two never coexist (delay lines take much of SRAM).
    processor_.ClearModel();

    // Build and prepare the new model; on any failure audio stays in bypass.
    try {
        ready_ = processor_.SetModel(CreateAmpModel(amp), kSampleRate, kBlockSize);
    } catch (...) {
        // Allocation/construction failure must not prevent audio startup.
    }
    return ready_;
}

void NamAudio::Process(const float *input_left, float *output_left, float *output_right, std::size_t frame_count, bool bypass_model) {
    // Run the model, or pass the input through if bypassed, not loaded, or the block is too big.
    const bool processed = !bypass_model && ProcessModel(input_left, frame_count);
    const float *source = processed ? output_.data() : input_left;

    // Apply output gain and clamp, and copy the mono signal to both channels.
    for (std::size_t i = 0; i < frame_count; ++i) {
        const float sample = ApplyOutputGainAndClamp(source[i]);
        output_left[i] = sample;
        output_right[i] = sample;
    }
}

bool NamAudio::ProcessModel(const float *input, std::size_t frame_count) {
    if (!ready_ || frame_count > kBlockSize)
        return false;

    // Copy the input in with gain, replacing NaN and infinity with silence.
    for (std::size_t i = 0; i < frame_count; ++i)
        input_[i] = std::isfinite(input[i]) ? input[i] * kInputGain : 0.0f;

    return processor_.Process(input_.data(), output_.data(), frame_count);
}
