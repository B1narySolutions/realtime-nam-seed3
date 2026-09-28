// Usage: a2_test <reference.f32>... with one upstream rendering per AmpId, in order.
#include "allocation_guard.h"
#include "audio/nam_processor.h"
#include "audio_stimulus.h"
#include "models/amp_models.h"
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

// Mix block sizes of 1, 17, and 48 frames so ring buffers wrap at odd offsets.
static int NextBlockSize(int offset) {
    const int frames = offset % 5 == 0 ? 1 : (offset % 3 == 0 ? 48 : 17);
    return std::min(frames, kTestSamples - offset);
}

int main(int argc, char **argv) {
    assert(argc == 4);
    for (int id = 1; id <= 3; ++id) {
        const std::vector<float> reference = ReadReference(argv[id]);
        NamProcessor processor;
        assert(processor.SetModel(CreateAmpModel(static_cast<AmpId>(id)), 48000, 48));
        std::array<float, 48> input{}, output{};
        float maximum = 0;
        double error = 0, energy = 0;
        for (int offset = 0; offset < kTestSamples;) {
            const int frames = NextBlockSize(offset);
            for (int i = 0; i < frames; ++i)
                input[i] = TestInput(offset + i);
            const auto allocations = allocation_count;
            assert(processor.Process(input.data(), output.data(), frames));
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
        std::cout << AmpName(static_cast<AmpId>(id)) << ": max error=" << maximum << " ESR=" << error / energy << std::endl;
        assert(maximum < 1e-4 && error / energy < 1e-8);
        // Reinitialization must reset the entire convolution history.
        assert(processor.SetModel(CreateAmpModel(static_cast<AmpId>(id)), 48000, 48));
        input.fill(0);
        assert(processor.Process(input.data(), output.data(), 48));
        for (int i = 0; i < 48; ++i)
            assert(std::fabs(output[i] - reference[i]) < 1e-4);
    }
}
