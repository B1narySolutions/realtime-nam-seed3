// Renders a mono float32 file through one amp model, for the figures in viz/.
//
// Usage: render engine <amp-id> <input.f32> <output.f32>
//        render engine-cab <amp-id> <input.f32> <output.f32>
//        render cab none <input.f32> <output.f32>
//        render upstream <model.nam> <input.f32> <output.f32>
//
// "engine" runs this firmware's A2-Lite engine on the embedded weights, in
// the firmware's 48-frame blocks. "engine-cab" follows it with the firmware's
// cabinet EQ, as NamAudio does, and "cab" runs the cabinet EQ alone. Neither
// applies NamAudio's output gain or clamp. "upstream" runs NAM Core's generic
// WaveNet on the original .nam file, the same reference make test compares
// against.
#include "NAM/get_dsp.h"
#include "audio/cab_filter.h"
#include "models/amp_models.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

static constexpr int kBlockSize = 48;

static std::vector<float> ReadSamples(const char *path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<float> samples(bytes.size() / sizeof(float));
    std::memcpy(samples.data(), bytes.data(), samples.size() * sizeof(float));
    return samples;
}

// Calls process(input, output, frames) over the whole signal in firmware-sized
// blocks; the last block may be shorter.
template <typename Process> static std::vector<float> RenderBlocks(const std::vector<float> &input, Process process) {
    std::vector<float> output(input.size());
    for (std::size_t offset = 0; offset < input.size(); offset += kBlockSize) {
        const int frames = static_cast<int>(std::min<std::size_t>(kBlockSize, input.size() - offset));
        process(&input[offset], &output[offset], frames);
    }
    return output;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        std::cerr << "usage: render engine <amp-id> | engine-cab <amp-id> | cab none | upstream <model.nam>  <input.f32> <output.f32>\n";
        return 2;
    }
    const std::string kind = argv[1];
    const std::vector<float> input = ReadSamples(argv[3]);
    std::vector<float> output;

    // Firmware engine: the same model the Seed builds, reset and prewarmed,
    // optionally followed by the cabinet.
    if (kind == "engine" || kind == "engine-cab") {
        auto model = CreateAmpModel(static_cast<AmpId>(std::atoi(argv[2])));
        if (!model) {
            std::cerr << "unknown amp id " << argv[2] << "\n";
            return 2;
        }
        model->Reset(kBlockSize);
        CabFilter cabinet;
        const bool with_cabinet = kind == "engine-cab";
        output = RenderBlocks(input, [&](const float *in, float *out, int frames) {
            model->Process(in, out, frames);
            if (with_cabinet)
                cabinet.Process(out, frames);
        });
    }

    // Firmware cabinet EQ on its own.
    else if (kind == "cab") {
        CabFilter cabinet;
        output = RenderBlocks(input, [&](const float *in, float *out, int frames) {
            std::copy(in, in + frames, out);
            cabinet.Process(out, frames);
        });
    }

    // Upstream reference: NAM Core's Reset also prewarms by default.
    else if (kind == "upstream") {
        auto model = nam::get_dsp(std::filesystem::path(argv[2]));
        model->Reset(48000.0, kBlockSize);
        output = RenderBlocks(input, [&](const float *in, float *out, int frames) {
            float *in_channel = const_cast<float *>(in);
            model->process(&in_channel, &out, frames);
        });
    } else {
        std::cerr << "unknown renderer " << kind << "\n";
        return 2;
    }

    // Raw native-endian float32, the same format as the test references.
    std::ofstream file(argv[4], std::ios::binary);
    file.write(reinterpret_cast<const char *>(output.data()), output.size() * sizeof(float));
    return file.good() ? 0 : 1;
}
