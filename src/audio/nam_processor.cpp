#include "audio/nam_processor.h"

#include <cmath>
#include <limits>
#include <utility>

NamProcessor::NamProcessor() = default;
NamProcessor::~NamProcessor() = default;

void NamProcessor::ClearModel() {
    model_.reset();
    max_block_size_ = 0;
}

bool NamProcessor::SetModel(std::unique_ptr<AmpModel> model, double sample_rate_hz, std::size_t max_block_size) {
    // Reject a missing model or an invalid sample rate.
    if (!model || !std::isfinite(sample_rate_hz) || sample_rate_hz <= 0.0)
        return false;

    // Reject block sizes the model cannot take.
    if (max_block_size == 0 || max_block_size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return false;

    // No resampling: the model's rate must match the audio rate.
    if (model->SampleRate() != sample_rate_hz)
        return false;

    // Allocate and prewarm the model; it may throw.
    try {
        model->Reset(static_cast<int>(max_block_size));
    } catch (...) {
        return false;
    }
    // Commit the replacement only after prewarming succeeds.
    model_ = std::move(model);
    max_block_size_ = max_block_size;
    return true;
}

bool NamProcessor::Process(const float *input, float *output, std::size_t frame_count) {
    // Refuse unless a model is loaded and the buffers and block size are valid.
    if (!model_ || !input || !output || input == output || frame_count == 0 || frame_count > max_block_size_) {
        return false;
    }

    model_->Process(input, output, static_cast<int>(frame_count));
    return true;
}
