#pragma once

#include "models/amp_model.h"
#include <cstddef>
#include <memory>

// Owns one mono model. SetModel/destruction require audio to be stopped;
// Process is the only method intended for the audio callback.
class NamProcessor {
  public:
    NamProcessor();
    ~NamProcessor();

    NamProcessor(const NamProcessor &) = delete;
    NamProcessor &operator=(const NamProcessor &) = delete;

    // Releases the current model so its memory can be reused by the next one.
    void ClearModel();

    // Takes ownership, validates format, then resets and prewarms the model.
    // Returns false on failure, preserving any previously prepared model.
    // A model whose rate differs from sample_rate_hz is rejected; no resampling is performed.
    bool SetModel(std::unique_ptr<AmpModel> model, double sample_rate_hz, std::size_t max_block_size);

    // Input and output must be distinct buffers of at least frame_count samples.
    // Returns false without writing output if unprepared or arguments invalid.
    // No wrapper allocations; model runtime behavior must be validated
    // separately.
    bool Process(const float *input, float *output, std::size_t frame_count);

  private:
    std::unique_ptr<AmpModel> model_;
    std::size_t max_block_size_ = 0;
};
