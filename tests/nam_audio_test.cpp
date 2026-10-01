#include "allocation_guard.h"
#include "audio/cab_filter.h"
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

static std::vector<float> ReadReference(const char *path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<float> reference(kTestSamples);
    file.read(reinterpret_cast<char *>(reference.data()), reference.size() * sizeof(float));
    assert(file.gcount() == static_cast<std::streamsize>(reference.size() * sizeof(float)));
    return reference;
}

// Usage: nam_audio_test <reference.f32>... with one upstream rendering per
// AmpId, in models/amps.json order.
int main(int argc, char **argv) {
    assert(argc == kAmpCount + 1);
    NamAudio audio;
    std::array<float, 49> input{}, left{}, right{};
    input.fill(0.5f);
    audio.Process(input.data(), left.data(), right.data(), 49, false);
    assert(left[48] == 0.4f && right[48] == 0.4f);
    // Reuse the audio path across switches to every amp, then back to the first.
    for (int step = 0; step <= kAmpCount; ++step) {
        const int id = step % kAmpCount + 1;
        // The cabinet is on by default, so expect upstream's output through it.
        std::vector<float> reference = ReadReference(argv[id]);
        CabFilter().Process(reference.data(), reference.size());
        assert(audio.LoadAmpModel(static_cast<AmpId>(id)));
        input.fill(0.5f);
        audio.Process(input.data(), left.data(), right.data(), 49, false);
        assert(left[48] == 0.4f && right[48] == 0.4f);
        bool bypassed = false;
        for (int offset = 0; offset < kTestSamples;) {
            // Bypass skips the model, so its state must not advance: after
            // unrelated bypassed input, the stream resumes where it left off.
            if (offset >= 12000 && !bypassed) {
                bypassed = true;
                for (int block = 0; block < 80; ++block) {
                    for (int i = 0; i < 48; ++i)
                        input[i] = 1.5f * TestInput(block * 48 + i + 7);
                    left.fill(123.0f);
                    right.fill(123.0f);
                    const auto allocations = allocation_count;
                    audio.Process(input.data(), left.data(), right.data(), 48, true);
                    assert(allocation_count == allocations);
                    for (int i = 0; i < 48; ++i)
                        assert(left[i] == std::clamp(input[i] * NamAudio::kOutputGain, -1.0f, 1.0f) && left[i] == right[i]);
                    assert(left[48] == 123.0f && right[48] == 123.0f);
                }
            }
            const int frames = std::min(offset % 3 == 0 ? 48 : 17, kTestSamples - offset);
            for (int i = 0; i < frames; ++i)
                input[i] = TestInput(offset + i);
            left.fill(123.0f);
            right.fill(123.0f);
            const auto allocations = allocation_count;
            audio.Process(input.data(), left.data(), right.data(), frames, false);
            assert(allocation_count == allocations);
            for (int i = 0; i < frames; ++i) {
                const float expected = std::clamp(reference[offset + i] * NamAudio::kOutputGain, -1.0f, 1.0f);
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
    // With the cabinet off, the output is upstream's again.
    const std::vector<float> raw_reference = ReadReference(argv[1]);
    assert(audio.LoadAmpModel(static_cast<AmpId>(1)));
    audio.SetCabinet(false);
    for (int offset = 0; offset < kTestSamples; offset += 48) {
        for (int i = 0; i < 48; ++i)
            input[i] = TestInput(offset + i);
        audio.Process(input.data(), left.data(), right.data(), 48, false);
        for (int i = 0; i < 48; ++i)
            assert(std::fabs(left[i] - std::clamp(raw_reference[offset + i] * NamAudio::kOutputGain, -1.0f, 1.0f)) < 1e-4f);
    }
    audio.SetCabinet(true);
    assert(!audio.LoadAmpModel(static_cast<AmpId>(99)));
    input.fill(0.5f);
    audio.Process(input.data(), left.data(), right.data(), 48, false);
    assert(left[0] == 0.4f && right[47] == 0.4f);
    std::cout << "A2 audio: switching, bypass, cabinet, bounds, and no processing allocations passed\n";
}
