// Usage: engine_digest_test <digests.txt> [--update]
//
// Checks that the engine's output is bit-identical to the digests saved in
// tests/engine_digests.txt, for each amp alone and through the whole audio
// path (trims, cabinet, knobs). a2_test only checks a tolerance against
// upstream NAM; this catches any change to the bits, such as a reordered sum.
// When a change is meant to alter the output, rerun with --update (make
// update-engine-digests) and say why in the commit.
//
// The input uses integer arithmetic only, and the test builds with
// -ffp-contract=off, so the digests match across compilers and platforms.
// They describe the host build; the firmware's compiler fuses multiply-adds,
// so its bits differ.
#include "audio/nam_audio.h"
#include "models/amp_models.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

static constexpr int kSamples = 24000;
static constexpr int kBlock = static_cast<int>(NamAudio::kBlockSize);

// Silence, an impulse, a step, then noise and a triangle wave at several
// levels, up to past full scale.
static float DigestInput(int sample) {
    if (sample < 2400)
        return 0.0f;
    if (sample == 2400)
        return 1.0f;
    if (sample < 4800)
        return 0.0f;
    if (sample < 7200)
        return 0.25f;

    // xorshift32 seeded by the sample index, scaled to [-1, 1).
    uint32_t x = static_cast<uint32_t>(sample) * 2654435761u + 1u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    const float noise = static_cast<float>(static_cast<int32_t>(x)) * (1.0f / 2147483648.0f);
    const int phase = sample % 200;
    const float triangle = static_cast<float>(phase < 100 ? phase - 50 : 150 - phase) / 50.0f;
    const float level = sample < 12000 ? 0.05f : sample < 18000 ? 0.4f : 1.5f;
    return level * (0.5f * noise + 0.5f * triangle);
}

// FNV-1a over each sample's bits.
static uint64_t Digest(const std::vector<float> &samples) {
    uint64_t hash = 14695981039346656037ull;
    for (float sample : samples) {
        uint32_t bits;
        std::memcpy(&bits, &sample, sizeof(bits));
        for (int byte = 0; byte < 4; ++byte) {
            hash ^= (bits >> (8 * byte)) & 0xffu;
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

static uint64_t EngineDigest(AmpId amp) {
    auto model = CreateAmpModel(amp);
    model->Reset(kBlock);
    std::vector<float> input(kSamples), output(kSamples);
    for (int i = 0; i < kSamples; ++i)
        input[i] = DigestInput(i);
    for (int offset = 0; offset < kSamples; offset += kBlock)
        model->Process(&input[offset], &output[offset], kBlock);
    return Digest(output);
}

// The whole path with the amp's default cabinet, and a knob move halfway so
// the glide and tone filter are covered too.
static uint64_t ChainDigest(AmpId amp) {
    auto audio = std::make_unique<NamAudio>();
    if (!audio->LoadAmpModel(amp))
        return 0;
    std::vector<float> input(kSamples), left(kSamples), right(kSamples);
    for (int i = 0; i < kSamples; ++i)
        input[i] = DigestInput(i);
    for (int offset = 0; offset < kSamples; offset += kBlock) {
        if (offset == kSamples / 2)
            audio->SetKnobs(0.6f, 0.4f);
        audio->Process(&input[offset], &left[offset], &right[offset], kBlock, false);
    }
    return Digest(left);
}

static std::string Hex(uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

int main(int argc, char **argv) {
    if (argc < 2 || (argc == 3 && std::strcmp(argv[2], "--update") != 0) || argc > 3) {
        std::cerr << "usage: engine_digest_test <digests.txt> [--update]\n";
        return 2;
    }
    const bool update = argc == 3;

    // One line per amp: name, engine digest, whole-path digest.
    std::ostringstream actual;
    for (int id = 1; id <= kAmpCount; ++id) {
        const AmpId amp = static_cast<AmpId>(id);
        actual << AmpName(amp) << '\t' << Hex(EngineDigest(amp)) << '\t' << Hex(ChainDigest(amp)) << '\n';
    }

    // Save new digests, or compare with the saved ones.
    if (update) {
        std::ofstream(argv[1]) << actual.str();
        std::cout << "Engine digests saved to " << argv[1] << '\n';
        return 0;
    }
    std::ifstream file(argv[1]);
    std::stringstream expected;
    expected << file.rdbuf();
    if (expected.str() != actual.str()) {
        std::cerr << "Engine output changed bit for bit.\nExpected (" << argv[1] << "):\n"
                  << expected.str() << "Actual:\n"
                  << actual.str()
                  << "If the change is meant to alter the output, run make update-engine-digests\n"
                     "and explain why in the commit. Otherwise the change has altered the engine's math.\n";
        return 1;
    }
    std::cout << "Engine and audio path: bit-identical to " << argv[1] << '\n';
    return 0;
}
