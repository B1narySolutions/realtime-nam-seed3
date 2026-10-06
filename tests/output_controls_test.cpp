#include "allocation_guard.h"
#include "audio/output_controls.h"
#include "audio_stimulus.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <numbers>
#include <vector>

// Steady-state gain in dB for a sine at a fixed tone reading, volume full.
static double ToneGainDb(float tone, double frequency_hz) {
    OutputControls controls;
    controls.SetKnobs(1.0f, tone);
    controls.Reset();
    constexpr int kSettle = 4800, kMeasure = 48000;
    std::vector<float> samples(kSettle + kMeasure);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<float>(0.25 * std::sin(2.0 * std::numbers::pi * frequency_hz * i / 48000.0));
    controls.Process(samples.data(), samples.size());
    double energy = 0.0;
    for (int i = kSettle; i < kSettle + kMeasure; ++i)
        energy += static_cast<double>(samples[i]) * samples[i];
    return 20.0 * std::log10(std::sqrt(energy / kMeasure) / (0.25 / std::sqrt(2.0)));
}

// Runs a block of a constant through and returns the last output sample.
static float SteadyOutput(OutputControls &controls, float value, int blocks = 1) {
    std::vector<float> block(48);
    for (int b = 0; b < blocks; ++b) {
        std::fill(block.begin(), block.end(), value);
        controls.Process(block.data(), block.size());
    }
    return block.back();
}

static std::vector<float> Stimulus() {
    std::vector<float> samples(kTestSamples);
    for (int i = 0; i < kTestSamples; ++i)
        samples[i] = TestInput(i);
    return samples;
}

int main() {
    // Untouched, and with both knobs fully up, the signal passes bit-exactly.
    const std::vector<float> input = Stimulus();
    std::vector<float> samples = input;
    OutputControls untouched;
    const auto allocations = allocation_count;
    untouched.Process(samples.data(), samples.size());
    assert(allocation_count == allocations);
    assert(samples == input);
    OutputControls full;
    full.SetKnobs(0.99f, 0.99f);
    full.Reset();
    samples = input;
    full.Process(samples.data(), samples.size());
    assert(samples == input);

    // After turning the knobs down and back up, it settles to bit-exact again.
    full.SetKnobs(0.3f, 0.3f);
    SteadyOutput(full, 0.5f, 200);
    full.SetKnobs(1.0f, 1.0f);
    SteadyOutput(full, 0.5f, 400);
    samples = input;
    for (int offset = 0; offset < kTestSamples; offset += 48)
        full.Process(samples.data() + offset, std::min(48, kTestSamples - offset));
    assert(samples == input);

    // Volume follows a cubic taper: half way is -18 dB, the bottom is silence.
    OutputControls volume;
    volume.SetKnobs(0.5f, 1.0f);
    volume.Reset();
    assert(std::fabs(SteadyOutput(volume, 0.5f) - 0.5f * 0.125f) < 1e-6f);
    volume.SetKnobs(0.01f, 1.0f);
    volume.Reset();
    assert(SteadyOutput(volume, 0.5f) == 0.0f);

    // Turning the volume down glides over tens of milliseconds, with no step.
    OutputControls glide;
    glide.SetKnobs(0.0f, 1.0f);
    std::vector<float> ramp(9600, 1.0f);
    for (std::size_t offset = 0; offset < ramp.size(); offset += 48)
        glide.Process(ramp.data() + offset, 48);
    assert(ramp[47] > 0.5f && ramp[960] > 0.01f && ramp.back() == 0.0f);
    for (std::size_t i = 1; i < ramp.size(); ++i)
        assert(ramp[i] <= ramp[i - 1] + 1e-9f && ramp[i - 1] - ramp[i] < 0.01f);

    // Tone: fully down is a dark low-pass, the middle keeps the guitar range,
    // and fully up is flat.
    assert(ToneGainDb(0.0f, 100.0) > -1.0);
    assert(ToneGainDb(0.0f, 2000.0) < -25.0);
    assert(std::fabs(ToneGainDb(0.5f, 500.0)) < 1.0);
    assert(ToneGainDb(0.5f, 8000.0) < -15.0);
    assert(std::fabs(ToneGainDb(1.0f, 15000.0)) < 1e-6);

    // Turning the tone up only ever lets more treble through.
    double previous = -1000.0;
    for (int step = 0; step <= 50; ++step) {
        const double gain = ToneGainDb(step / 50.0f, 4000.0);
        assert(gain >= previous - 1e-3);
        previous = gain;
    }

    // Stable at every tone position: an impulse rings out within 50 ms.
    for (int step = 0; step <= 100; ++step) {
        OutputControls controls;
        controls.SetKnobs(1.0f, step / 100.0f);
        controls.Reset();
        std::vector<float> impulse(2400, 0.0f);
        impulse[0] = 1.0f;
        controls.Process(impulse.data(), impulse.size());
        for (std::size_t i = 0; i < impulse.size(); ++i)
            assert(std::isfinite(impulse[i]) && std::fabs(impulse[i]) <= 1.0f);
        assert(std::fabs(impulse.back()) < 1e-6f);
    }

    // Sweeping both knobs while playing stays bounded.
    OutputControls sweep;
    samples = input;
    for (int offset = 0, block = 0; offset < kTestSamples; ++block) {
        const float position = 0.5f + 0.5f * std::sin(block * 0.37f);
        sweep.SetKnobs(position, 1.0f - position);
        const int frames = std::min(block % 3 == 0 ? 48 : 17, kTestSamples - offset);
        sweep.Process(samples.data() + offset, frames);
        offset += frames;
    }
    for (float sample : samples)
        assert(std::isfinite(sample) && std::fabs(sample) < 1.5f);

    // Reset forgets the earlier signal: an impulse then matches a fresh one's.
    OutputControls fresh;
    fresh.SetKnobs(0.7f, 0.2f);
    fresh.Reset();
    sweep.SetKnobs(0.7f, 0.2f);
    sweep.Reset();
    std::vector<float> fresh_impulse(480, 0.0f), reset_impulse(480, 0.0f);
    fresh_impulse[0] = reset_impulse[0] = 1.0f;
    fresh.Process(fresh_impulse.data(), fresh_impulse.size());
    sweep.Process(reset_impulse.data(), reset_impulse.size());
    assert(fresh_impulse == reset_impulse);

    std::cout << "Output controls: pass-through, volume taper, glide, tone response, stability, and reset passed\n";
}
