#pragma once

// Mono amp model at a fixed sample rate. Reset may allocate and throw;
// Process must not allocate, and is noexcept so a throw from it stops the
// program rather than unwinding through the audio callback.
class AmpModel {
  public:
    virtual ~AmpModel() = default;
    virtual double SampleRate() const = 0;
    // Sizes buffers for blocks of up to max_block_size frames and settles the
    // model into the state it reaches after unbounded silence.
    virtual void Reset(int max_block_size) = 0;
    virtual void Process(const float *input, float *output, int frame_count) noexcept = 0;
};
