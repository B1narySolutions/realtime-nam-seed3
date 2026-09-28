#include "audio/nam_audio.h"
#include <algorithm>
#include <cmath>

namespace {
float ApplyOutputGainAndClamp(float sample) {
    if (!std::isfinite(sample))
        return 0.0f;
    return std::max(-1.0f, std::min(1.0f, sample * NamAudio::kOutputGain));
}
} // namespace

bool NamAudio::LoadAmpModel(AmpId amp) {
    ready_ = false;
    // Two models never coexist; the delay lines are a large share of SRAM.
    processor_.ClearModel();
    try {
        ready_ = processor_.SetModel(CreateAmpModel(amp), kSampleRate, kBlockSize);
    } catch (...) {
        // Allocation/construction failure must not prevent audio startup.
    }
    return ready_;
}

void NamAudio::Process(const float *input_left, float *output_left, float *output_right, std::size_t frame_count, bool bypass_model) {
    const bool processed = ProcessModel(input_left, frame_count);
    const float *source = processed && !bypass_model ? output_.data() : input_left;
    for (std::size_t i = 0; i < frame_count; ++i) {
        const float sample = ApplyOutputGainAndClamp(source[i]);
        output_left[i] = sample;
        output_right[i] = sample;
    }
}

bool NamAudio::ProcessModel(const float *input, std::size_t frame_count) {
    if (!ready_ || frame_count > kBlockSize)
        return false;

    for (std::size_t i = 0; i < frame_count; ++i)
        input_[i] = std::isfinite(input[i]) ? input[i] * kInputGain : 0.0f;
    return processor_.Process(input_.data(), output_.data(), frame_count);
}
