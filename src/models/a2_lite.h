#pragma once

#include "models/amp_model.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Shape of the A2-Lite WaveNet. convert_a2.py validates that every embedded
// model matches it, so the engine never parses configuration at runtime.
namespace a2_lite {
constexpr int kChannels = 3;
constexpr int kLayers = 23;
constexpr int kHeadTaps = 16;
constexpr std::array<int, kLayers> kKernelSizes = {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 15, 15, 6, 6, 6, 6, 6, 6, 6};
constexpr std::array<int, kLayers> kDilations = {1, 3, 7, 17, 41, 101, 239, 1, 3, 7, 17, 41, 101, 239, 1, 13, 1, 3, 7, 17, 41, 101, 239};

// Weight stream produced by convert_a2.py:
//   input projection[kChannels]
//   per layer: taps[kernel_size][in][out], bias[kChannels], conditioning[kChannels],
//              residual[out][in], residual bias[kChannels]
//   head taps[kHeadTaps][kChannels], head bias, output scale
constexpr int kWeightsPerTap = kChannels * kChannels;
constexpr int kLayerTailWeights = 3 * kChannels + kChannels * kChannels;

constexpr int LayerWeightCount(int kernel_size) { return kernel_size * kWeightsPerTap + kLayerTailWeights; }

constexpr std::size_t WeightCount() {
    std::size_t count = kChannels;
    for (int kernel_size : kKernelSizes)
        count += LayerWeightCount(kernel_size);
    return count + kHeadTaps * kChannels + 2;
}

// Samples needed to fill every delay line; matches upstream prewarming.
constexpr int ReceptiveField() {
    int samples = kHeadTaps;
    for (int i = 0; i < kLayers; ++i)
        samples += (kKernelSizes[i] - 1) * kDilations[i];
    return samples;
}
} // namespace a2_lite

// Fixed-shape A2-Lite inference. Weights must outlive this object (embedded
// flash). Buffers are allocated in Reset; process never allocates.
//
// Each layer's delay line only holds that layer's own input, so a block is
// processed layer by layer rather than sample by sample. Delay lines are
// mirrored ring buffers: every frame is stored twice, one period apart, so
// any window of past frames is contiguous and taps are plain offsets.
class A2Lite final : public AmpModel {
  public:
    static constexpr std::size_t kWeights = a2_lite::WeightCount();
    explicit A2Lite(const float *weights);
    double SampleRate() const override { return 48000.0; }
    void Reset(int max_block_size) override;
    void Process(const float *input, float *output, int frame_count) override;

  private:
    // Ring of `period` interleaved [frame][kChannels] frames, stored twice.
    struct DelayLine {
        std::vector<float> samples;
        int period = 0; // context frames plus one block
        int write = 0;  // next frame to write, in [0, period)

        void Resize(int context_frames, int max_block_size);
        void Clear();
        // Stores frames at the write position and returns that position.
        int Push(const float *frames, int frame_count);
        // Frames [index, index + frame_count) counted back from a Push result.
        const float *Window(int start, int frames_back) const {
            int index = start - frames_back;
            if (index < 0)
                index += period;
            return &samples[static_cast<std::size_t>(index) * a2_lite::kChannels];
        }
    };

    struct Layer {
        // Views into the weight stream; see the layout above.
        const float *taps = nullptr;
        const float *bias = nullptr;
        const float *conditioning = nullptr;
        const float *residual = nullptr;
        const float *residual_bias = nullptr;
        int kernel_size = 0;
        int dilation = 0;
        DelayLine history;
    };

    void ProcessLayer(Layer &layer, const float *conditioning, int frame_count);
    void ProcessHead(float *output, int frame_count);

    const float *input_projection_;
    std::array<Layer, a2_lite::kLayers> layers_;
    const float *head_taps_;
    float head_bias_;
    float head_scale_;
    DelayLine head_history_;
    // Per-block working set of [frame][kChannels] values.
    std::vector<float> residual_;
    std::vector<float> skip_;
    std::vector<float> activation_;
};
