// Usage: a2_test <reference.f32>... with one upstream rendering per AmpId, in order.
#include "allocation_guard.h"
#include "audio/nam_processor.h"
#include "audio_stimulus.h"
#include "models/amp_models.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <vector>

static std::vector<float> ReadReference(const char *path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<float> reference(kTestSamples);
    file.read(reinterpret_cast<char *>(reference.data()), reference.size() * sizeof(float));
    assert(file.gcount() == static_cast<std::streamsize>(reference.size() * sizeof(float)));
    return reference;
}

// Mix block sizes of 1, 17, 48, and the maximum so ring buffers wrap at odd
// offsets.
static int NextBlockSize(int block, int offset, int max_block_size) {
    static constexpr std::array<int, 5> kPattern = {1, 17, 0, 48, 17};
    const int frames = kPattern[block % kPattern.size()];
    return std::min({frames ? frames : max_block_size, max_block_size, kTestSamples - offset});
}

struct ReferenceError {
    float maximum = 0;
    double esr = 0;
};

// Streams the test input through `process` in blocks of up to max_block_size
// frames. Processing must not allocate, and the output must match the
// upstream rendering.
template <typename Process> static ReferenceError ExpectMatchesReference(const std::vector<float> &reference, int max_block_size, Process process) {
    std::vector<float> input(max_block_size), output(max_block_size);
    float maximum = 0;
    double error = 0, energy = 0;
    for (int block = 0, offset = 0; offset < kTestSamples; ++block) {
        const int frames = NextBlockSize(block, offset, max_block_size);
        for (int i = 0; i < frames; ++i)
            input[i] = TestInput(offset + i);
        const auto allocations = allocation_count;
        process(input.data(), output.data(), frames);
        assert(allocation_count == allocations);
        for (int i = 0; i < frames; ++i) {
            assert(std::isfinite(output[i]));
            double delta = output[i] - reference[offset + i];
            maximum = std::max(maximum, static_cast<float>(std::fabs(delta)));
            error += delta * delta;
            energy += reference[offset + i] * reference[offset + i];
        }
        offset += frames;
    }
    assert(maximum < 1e-4 && error / energy < 1e-8);
    return {maximum, error / energy};
}

// Reset must leave the model settled, so silence yields one constant output
// from the first sample regardless of block size.
static void ExpectSettledSilence(AmpId id) {
    for (int block : {1, 17, 48}) {
        auto model = CreateAmpModel(id);
        model->Reset(48);
        std::array<float, 48> silence{}, output{};
        model->Process(silence.data(), output.data(), block);
        const float settled = output[0];
        for (int offset = 0; offset < kTestSamples; offset += block) {
            if (offset)
                model->Process(silence.data(), output.data(), block);
            for (int i = 0; i < block; ++i)
                assert(output[i] == settled);
        }
    }
}

int main(int argc, char **argv) {
    assert(argc == 4);
    for (int id = 1; id <= 3; ++id) {
        const AmpId amp = static_cast<AmpId>(id);
        const std::vector<float> reference = ReadReference(argv[id]);
        ExpectSettledSilence(amp);

        // Ring buffer periods depend on the maximum block size.
        NamProcessor processor;
        for (int max_block_size : {1, 17, 48, 256}) {
            assert(processor.SetModel(CreateAmpModel(amp), 48000, max_block_size));
            const ReferenceError result = ExpectMatchesReference(reference, max_block_size, [&](const float *input, float *output, int frames) {
                assert(processor.Process(input, output, frames));
            });
            if (max_block_size == 48)
                std::cout << AmpName(amp) << ": max error=" << result.maximum << " ESR=" << result.esr << std::endl;
        }

        // Resetting a model that has processed audio, with a different block
        // size, must discard all of its state.
        auto model = CreateAmpModel(amp);
        model->Reset(48);
        std::array<float, 48> input{}, output{};
        for (int offset = 14400; offset < 19200; offset += 48) {
            for (int i = 0; i < 48; ++i)
                input[i] = TestInput(offset + i);
            model->Process(input.data(), output.data(), 48);
        }
        model->Reset(17);
        ExpectMatchesReference(reference, 17, [&](const float *in, float *out, int frames) { model->Process(in, out, frames); });

        // Replacing the processor's model must reset the entire convolution
        // history.
        assert(processor.SetModel(CreateAmpModel(amp), 48000, 48));
        input.fill(0);
        assert(processor.Process(input.data(), output.data(), 48));
        for (int i = 0; i < 48; ++i)
            assert(std::fabs(output[i] - reference[i]) < 1e-4);
    }
}
