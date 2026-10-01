#include "allocation_guard.h"
#include "audio/cab_filter.h"
#include "audio_stimulus.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <numbers>
#include <vector>

// Steady-state gain in dB for a sine, measured after the filter settles.
static double GainDb(double frequency_hz) {
    CabFilter filter;
    constexpr int kSettle = 24000, kMeasure = 48000;
    std::vector<float> samples(kSettle + kMeasure);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<float>(0.25 * std::sin(2.0 * std::numbers::pi * frequency_hz * i / 48000.0));
    filter.Process(samples.data(), samples.size());
    double energy = 0.0;
    for (int i = kSettle; i < kSettle + kMeasure; ++i)
        energy += static_cast<double>(samples[i]) * samples[i];
    const double rms = std::sqrt(energy / kMeasure);
    return 20.0 * std::log10(rms / (0.25 / std::sqrt(2.0)));
}

int main() {
    // Speaker-like response: rumble and fizz cut hard, the guitar range kept.
    assert(GainDb(30.0) < -12.0);
    assert(GainDb(110.0) > 0.0);
    assert(std::fabs(GainDb(1000.0)) < 2.0);
    assert(GainDb(2500.0) > 1.0);
    assert(GainDb(8000.0) < -12.0);
    assert(GainDb(12000.0) < -30.0);

    // Splitting the stream into odd blocks must give bit-identical output.
    std::vector<float> whole(kTestSamples), split(kTestSamples);
    for (int i = 0; i < kTestSamples; ++i)
        whole[i] = split[i] = TestInput(i);
    CabFilter whole_filter, split_filter;
    whole_filter.Process(whole.data(), whole.size());
    const auto allocations = allocation_count;
    for (int offset = 0; offset < kTestSamples;) {
        const int frames = std::min(offset % 3 == 0 ? 48 : 17, kTestSamples - offset);
        split_filter.Process(split.data() + offset, frames);
        offset += frames;
    }
    assert(allocation_count == allocations);
    for (int i = 0; i < kTestSamples; ++i)
        assert(whole[i] == split[i]);

    // Reset forgets the earlier signal: an impulse then matches a fresh filter's.
    std::vector<float> fresh(4800, 0.0f), reset(4800, 0.0f);
    fresh[0] = reset[0] = 1.0f;
    CabFilter().Process(fresh.data(), fresh.size());
    whole_filter.Reset();
    whole_filter.Process(reset.data(), reset.size());
    for (std::size_t i = 0; i < fresh.size(); ++i)
        assert(fresh[i] == reset[i] && std::isfinite(fresh[i]));

    // Stable: the impulse response dies away within 100 ms.
    for (std::size_t i = fresh.size() - 48; i < fresh.size(); ++i)
        assert(std::fabs(fresh[i]) < 1e-6f);

    std::cout << "Cabinet: response, block independence, reset, and stability passed\n";
}
