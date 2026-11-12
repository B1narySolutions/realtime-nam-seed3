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
        const AmpId amp = static_cast<AmpId>(id);
        const bool cabinet_default = !AmpIncludesCabinet(amp);
        // Each load turns the cabinet on, unless the capture has its own, so
        // expect upstream's output through it where it is on, and then the
        // amp's output trim. Flipping it before the load checks the reset.
        std::vector<float> reference = ReadReference(argv[id]);
        if (cabinet_default)
            CabFilter().Process(reference.data(), reference.size());
        for (float &sample : reference)
            sample *= AmpOutputGain(amp);
        audio.SetCabinet(!cabinet_default);
        assert(audio.LoadAmpModel(amp));
        assert(audio.CabinetEnabled() == cabinet_default);
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
            // Undo the amp's input trim, so the model sees upstream's input.
            const int frames = std::min(offset % 3 == 0 ? 48 : 17, kTestSamples - offset);
            for (int i = 0; i < frames; ++i)
                input[i] = TestInput(offset + i) / AmpInputGain(amp);
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
    // With the cabinet off, the output is upstream's again, after the trims.
    const AmpId first = static_cast<AmpId>(1);
    const std::vector<float> raw_reference = ReadReference(argv[1]);
    assert(audio.LoadAmpModel(first));
    audio.SetCabinet(false);
    for (int offset = 0; offset < kTestSamples; offset += 48) {
        for (int i = 0; i < 48; ++i)
            input[i] = TestInput(offset + i) / AmpInputGain(first);
        audio.Process(input.data(), left.data(), right.data(), 48, false);
        for (int i = 0; i < 48; ++i)
            assert(std::fabs(left[i] - std::clamp(raw_reference[offset + i] * AmpOutputGain(first) * NamAudio::kOutputGain, -1.0f, 1.0f)) < 1e-4f);
    }
    // An amp whose capture has its own cabinet can still have the EQ turned on.
    for (int id = 1; id <= kAmpCount; ++id) {
        const AmpId amp = static_cast<AmpId>(id);
        if (!AmpIncludesCabinet(amp))
            continue;
        std::vector<float> reference = ReadReference(argv[id]);
        CabFilter().Process(reference.data(), reference.size());
        assert(audio.LoadAmpModel(amp) && !audio.CabinetEnabled());
        audio.SetCabinet(true);
        for (int offset = 0; offset < kTestSamples; offset += 48) {
            for (int i = 0; i < 48; ++i)
                input[i] = TestInput(offset + i) / AmpInputGain(amp);
            audio.Process(input.data(), left.data(), right.data(), 48, false);
            for (int i = 0; i < 48; ++i)
                assert(std::fabs(left[i] - std::clamp(reference[offset + i] * AmpOutputGain(amp) * NamAudio::kOutputGain, -1.0f, 1.0f)) < 1e-4f);
        }
    }
    assert(!audio.LoadAmpModel(static_cast<AmpId>(99)));
    input.fill(0.5f);
    audio.Process(input.data(), left.data(), right.data(), 48, false);
    assert(left[0] == 0.4f && right[47] == 0.4f);
    // The knobs apply in bypass too: fully down is silence, half way -18 dB.
    audio.SetKnobs(0.0f, 1.0f);
    audio.ResetKnobs();
    audio.Process(input.data(), left.data(), right.data(), 48, true);
    assert(left[0] == 0.0f && right[47] == 0.0f);
    audio.SetKnobs(0.5f, 1.0f);
    audio.ResetKnobs();
    audio.Process(input.data(), left.data(), right.data(), 48, true);
    assert(std::fabs(left[47] - 0.4f * 0.125f) < 1e-6f && left[47] == right[47]);
    // NaN and infinity never reach the tone filter, so they can't stick in it.
    audio.SetKnobs(1.0f, 0.5f);
    audio.ResetKnobs();
    input[0] = std::numeric_limits<float>::quiet_NaN();
    input[1] = std::numeric_limits<float>::infinity();
    audio.Process(input.data(), left.data(), right.data(), 48, true);
    input.fill(0.5f);
    audio.Process(input.data(), left.data(), right.data(), 48, true);
    for (int i = 0; i < 48; ++i)
        assert(std::isfinite(left[i]) && left[i] > 0.0f);
    std::cout << "A2 audio: switching, bypass, cabinet, knobs, bounds, and no processing allocations passed\n";
}
