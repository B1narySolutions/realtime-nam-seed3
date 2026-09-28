// A2-Lite equations and weight ordering follow NeuralAmpModelerCore's WaveNet.
// See NAM-LICENSE for the upstream MIT notice.
#include "models/a2_lite.h"
#include <algorithm>

using namespace a2_lite;

void A2Lite::DelayLine::Resize(int context_frames, int max_block_size) {
    period = context_frames + max_block_size;
    samples.assign(static_cast<std::size_t>(2 * period) * kChannels, 0.0f);
    write = 0;
}

void A2Lite::DelayLine::Clear() {
    std::fill(samples.begin(), samples.end(), 0.0f);
    write = 0;
}

int A2Lite::DelayLine::Push(const float *frames, int frame_count) {
    const int start = write;
    float *primary = &samples[static_cast<std::size_t>(write) * kChannels];
    float *mirror = primary + static_cast<std::size_t>(period) * kChannels;
    for (int t = 0; t < frame_count; ++t, frames += kChannels) {
        // Wrap within the period; the mirror copy follows one period later.
        if (write == period) {
            write = 0;
            primary = samples.data();
            mirror = primary + static_cast<std::size_t>(period) * kChannels;
        }
        primary[0] = mirror[0] = frames[0];
        primary[1] = mirror[1] = frames[1];
        primary[2] = mirror[2] = frames[2];
        primary += kChannels;
        mirror += kChannels;
        ++write;
    }
    if (write == period)
        write = 0;
    return start;
}

A2Lite::A2Lite(const float *weights) : input_projection_(weights) {
    const float *next = weights + kChannels;
    for (std::size_t i = 0; i < layers_.size(); ++i) {
        auto &layer = layers_[i];
        layer.kernel_size = kKernelSizes[i];
        layer.dilation = kDilations[i];
        layer.taps = next;
        layer.bias = layer.taps + layer.kernel_size * kWeightsPerTap;
        layer.conditioning = layer.bias + kChannels;
        layer.residual = layer.conditioning + kChannels;
        layer.residual_bias = layer.residual + kChannels * kChannels;
        next += LayerWeightCount(layer.kernel_size);
    }
    head_taps_ = next;
    head_bias_ = next[kHeadTaps * kChannels];
    head_scale_ = next[kHeadTaps * kChannels + 1];
}

void A2Lite::Reset(int max_block_size) {
    const int block = std::max(max_block_size, 1);
    for (auto &layer : layers_)
        layer.history.Resize((layer.kernel_size - 1) * layer.dilation, block);
    head_history_.Resize(kHeadTaps - 1, block);
    residual_.assign(static_cast<std::size_t>(block) * kChannels, 0.0f);
    skip_.assign(static_cast<std::size_t>(block) * kChannels, 0.0f);
    activation_.assign(static_cast<std::size_t>(block) * kChannels, 0.0f);
    // Process silence until every delay line is full, as upstream prewarming does.
    std::vector<float> silence(static_cast<std::size_t>(block), 0.0f), output(silence.size());
    for (int frames = 0; frames < ReceptiveField(); frames += block)
        Process(silence.data(), output.data(), block);
}

// Accumulates one or two taps across a block. Everything hot is held in
// local scalars, so stores to the accumulator never force weight or history
// reloads. Each output channel sums taps and inputs in upstream order.
template <int kTaps> static void AccumulateTaps(float *activation, const float *weights, const float *history, int history_stride, int frame_count) {
    float w[kTaps * kWeightsPerTap];
    for (int i = 0; i < kTaps * kWeightsPerTap; ++i)
        w[i] = weights[i];
    for (int t = 0; t < frame_count; ++t, activation += kChannels, history += kChannels) {
        float a0 = activation[0], a1 = activation[1], a2 = activation[2];
        for (int tap = 0; tap < kTaps; ++tap) {
            const float *h = history + tap * history_stride;
            const float h0 = h[0], h1 = h[1], h2 = h[2];
            const float *tw = w + tap * kWeightsPerTap;
            a0 += tw[0] * h0;
            a0 += tw[3] * h1;
            a0 += tw[6] * h2;
            a1 += tw[1] * h0;
            a1 += tw[4] * h1;
            a1 += tw[7] * h2;
            a2 += tw[2] * h0;
            a2 += tw[5] * h1;
            a2 += tw[8] * h2;
        }
        activation[0] = a0;
        activation[1] = a1;
        activation[2] = a2;
    }
}

void A2Lite::ProcessLayer(Layer &layer, const float *conditioning, int frame_count) {
    const int start = layer.history.Push(residual_.data(), frame_count);

    float *activation = activation_.data();
    const float b0 = layer.bias[0], b1 = layer.bias[1], b2 = layer.bias[2];
    for (int t = 0; t < frame_count; ++t) {
        activation[t * kChannels + 0] = b0;
        activation[t * kChannels + 1] = b1;
        activation[t * kChannels + 2] = b2;
    }
    // Dilated convolution. Tap i reads (kernel_size - 1 - i) * dilation
    // frames back; consecutive taps are one dilation apart.
    const int history_stride = layer.dilation * kChannels;
    int tap = 0;
    for (; tap + 2 <= layer.kernel_size; tap += 2)
        AccumulateTaps<2>(activation, layer.taps + tap * kWeightsPerTap, layer.history.Window(start, (layer.kernel_size - 1 - tap) * layer.dilation), history_stride, frame_count);
    if (tap < layer.kernel_size)
        AccumulateTaps<1>(activation, layer.taps + tap * kWeightsPerTap, layer.history.Window(start, (layer.kernel_size - 1 - tap) * layer.dilation), history_stride, frame_count);

    // Input conditioning, leaky ReLU, skip connection, and the 1x1 residual
    // projection back onto the layer input.
    const float c0 = layer.conditioning[0], c1 = layer.conditioning[1], c2 = layer.conditioning[2];
    const float rb0 = layer.residual_bias[0], rb1 = layer.residual_bias[1], rb2 = layer.residual_bias[2];
    float r[kWeightsPerTap];
    for (int i = 0; i < kWeightsPerTap; ++i)
        r[i] = layer.residual[i];
    float *residual = residual_.data();
    float *skip = skip_.data();
    for (int t = 0; t < frame_count; ++t, activation += kChannels, residual += kChannels, skip += kChannels) {
        const float x = conditioning[t];
        float a0 = activation[0] + c0 * x;
        float a1 = activation[1] + c1 * x;
        float a2 = activation[2] + c2 * x;
        a0 = a0 >= 0.0f ? a0 : 0.01f * a0;
        a1 = a1 >= 0.0f ? a1 : 0.01f * a1;
        a2 = a2 >= 0.0f ? a2 : 0.01f * a2;
        skip[0] += a0;
        skip[1] += a1;
        skip[2] += a2;
        residual[0] += rb0 + r[0] * a0 + r[1] * a1 + r[2] * a2;
        residual[1] += rb1 + r[3] * a0 + r[4] * a1 + r[5] * a2;
        residual[2] += rb2 + r[6] * a0 + r[7] * a1 + r[8] * a2;
    }
}

void A2Lite::ProcessHead(float *output, int frame_count) {
    const int start = head_history_.Push(skip_.data(), frame_count);

    const float bias = head_bias_;
    for (int t = 0; t < frame_count; ++t)
        output[t] = bias;
    for (int tap = 0; tap < kHeadTaps; ++tap) {
        const float w0 = head_taps_[tap * kChannels + 0], w1 = head_taps_[tap * kChannels + 1], w2 = head_taps_[tap * kChannels + 2];
        const float *history = head_history_.Window(start, kHeadTaps - 1 - tap);
        for (int t = 0; t < frame_count; ++t, history += kChannels) {
            float o = output[t];
            o += w0 * history[0];
            o += w1 * history[1];
            o += w2 * history[2];
            output[t] = o;
        }
    }
    const float scale = head_scale_;
    for (int t = 0; t < frame_count; ++t)
        output[t] *= scale;
}

void A2Lite::Process(const float *input, float *output, int frame_count) {
    const float *x = input;
    for (int t = 0; t < frame_count; ++t)
        for (int channel = 0; channel < kChannels; ++channel)
            residual_[t * kChannels + channel] = input_projection_[channel] * x[t];
    std::fill(skip_.begin(), skip_.begin() + static_cast<std::ptrdiff_t>(frame_count) * kChannels, 0.0f);
    for (auto &layer : layers_)
        ProcessLayer(layer, x, frame_count);
    ProcessHead(output, frame_count);
}
