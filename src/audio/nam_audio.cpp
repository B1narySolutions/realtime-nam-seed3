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

    // The new model starts from silence, so the cabinet should too. The EQ
    // starts on for amp-only captures and off for those with their own mic'd
    // speaker.
    cabinet_.Reset();
    cabinet_enabled_ = !AmpIncludesCabinet(amp);

    // Level the model with the amp's trims from models/amps.json.
    input_gain_ = AmpInputGain(amp);
    output_gain_ = AmpOutputGain(amp);

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

void NamAudio::SetCabinet(bool enabled) {
    cabinet_enabled_ = enabled;
    cabinet_.Reset();
}

void NamAudio::Process(const float *input_left, float *output_left, float *output_right, std::size_t frame_count, bool bypass_model) {
    // Run the model, or pass the input through if bypassed, not loaded, or the block is too big.
    const bool processed = !bypass_model && ProcessModel(input_left, frame_count);
    const float *source = processed ? output_.data() : input_left;

    // Shape the amp's output through the cabinet EQ when it is on; the clean
    // input skips it.
    if (processed && cabinet_enabled_)
        cabinet_.Process(output_.data(), frame_count);

    // Copy to the left output to work in place, replacing NaN and infinity
    // with silence so they can't stick in the tone filter's memory.
    for (std::size_t i = 0; i < frame_count; ++i)
        output_left[i] = std::isfinite(source[i]) ? source[i] : 0.0f;

    // Apply the volume and tone knobs, in bypass too.
    controls_.Process(output_left, frame_count);

    // Apply output gain and clamp, and copy the mono signal to both channels.
    for (std::size_t i = 0; i < frame_count; ++i) {
        const float sample = ApplyOutputGainAndClamp(output_left[i]);
        output_left[i] = sample;
        output_right[i] = sample;
    }
}

bool NamAudio::ProcessModel(const float *input, std::size_t frame_count) {
    if (!ready_ || frame_count > kBlockSize)
        return false;

    // Copy the input in with the amp's input trim, replacing NaN and infinity with silence.
    for (std::size_t i = 0; i < frame_count; ++i)
        input_[i] = std::isfinite(input[i]) ? input[i] * input_gain_ : 0.0f;

    // Run the model; if it fails, NamAudio::Process passes the input through.
    if (!processor_.Process(input_.data(), output_.data(), frame_count))
        return false;

    // Apply the amp's output trim so every amp plays at about the same loudness.
    for (std::size_t i = 0; i < frame_count; ++i)
        output_[i] *= output_gain_;
    return true;
}
