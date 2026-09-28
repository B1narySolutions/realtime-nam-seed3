#include "allocation_guard.h"
#include "audio/nam_audio.h"
#include "audio_stimulus.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

// Usage: nam_audio_test <reference.f32>... with one upstream rendering per AmpId, in order.
int main(int argc, char **argv) {
    assert(argc == 4);
    NamAudio audio;
    std::array<float, 49> input{}, left{}, right{};
    input.fill(0.5f);
    audio.Process(input.data(), left.data(), right.data(), 49, false);
    assert(left[48] == 0.4f && right[48] == 0.4f);
    // Reuse the audio path across switches, including switching back to Fender.
    for (int id : {1, 2, 3, 1}) {
        std::ifstream file(argv[id], std::ios::binary);
        std::vector<float> reference(kTestSamples);
        file.read(reinterpret_cast<char *>(reference.data()), reference.size() * sizeof(float));
        assert(file.gcount() == static_cast<std::streamsize>(reference.size() * sizeof(float)));
        assert(audio.LoadAmpModel(static_cast<AmpId>(id)));
        input.fill(0.5f);
        audio.Process(input.data(), left.data(), right.data(), 49, false);
        assert(left[48] == 0.4f && right[48] == 0.4f);
        for (int offset = 0; offset < kTestSamples;) {
            const int frames = std::min(offset % 3 == 0 ? 48 : 17, kTestSamples - offset);
            for (int i = 0; i < frames; ++i)
                input[i] = TestInput(offset + i);
            const bool bypass = offset >= 12000 && offset < 16000;
            left.fill(123.0f);
            right.fill(123.0f);
            const auto allocations = allocation_count;
            audio.Process(input.data(), left.data(), right.data(), frames, bypass);
            assert(allocation_count == allocations);
            for (int i = 0; i < frames; ++i) {
                const float expected = std::clamp((bypass ? input[i] : reference[offset + i]) * NamAudio::kOutputGain, -1.0f, 1.0f);
                assert(std::isfinite(left[i]) && left[i] == right[i]);
                assert(std::fabs(left[i] - expected) < 1e-4f);
            }
            assert(left[frames] == 123.0f && right[frames] == 123.0f);
            offset += frames;
        }
        input[0] = std::numeric_limits<float>::quiet_NaN();
        input[1] = std::numeric_limits<float>::infinity();
        input[2] = 100.0f;
        audio.Process(input.data(), left.data(), right.data(), 3, true);
        assert(left[0] == 0 && left[1] == 0 && left[2] == 1);
        audio.Process(input.data(), left.data(), right.data(), 3, false);
        for (int i = 0; i < 3; ++i)
            assert(std::isfinite(left[i]) && std::fabs(left[i]) <= 1);
    }
    assert(!audio.LoadAmpModel(static_cast<AmpId>(99)));
    input.fill(0.5f);
    audio.Process(input.data(), left.data(), right.data(), 48, false);
    assert(left[0] == 0.4f && right[47] == 0.4f);
    std::cout << "A2 audio: switching, bypass, bounds, and no processing allocations passed\n";
}
