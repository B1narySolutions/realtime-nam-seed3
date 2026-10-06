#include "audio/output_controls.h"
#include "audio/constexpr_math.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <numbers>

namespace {
// A little travel at each end counts as fully off or fully on, because ADC
// noise keeps the readings from sitting exactly at 0 or 1.
constexpr float kDeadband = 0.02f;

// Each block moves this fraction of the way to the target per frame it
// holds: a 20 ms time constant at 48 kHz.
constexpr float kGlidePerFrame = 1.0f / 960.0f;

// Within this of its target, a glide lands on it. A one-pole glide otherwise
// only approaches, and the fully open tone needs the exact end of the table.
constexpr float kGlideSnap = 1e-4f;

// Tone corners at third-octave steps from 400 Hz to 10 kHz. The last stretch
// of the knob opens from the 10 kHz corner to a pass-through.
constexpr double kToneCornersHz[] = {400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000};
constexpr std::size_t kToneSteps = std::size(kToneCornersHz) + 1;

// Each stage computes y += a * (x - y). a = 1 - e^(-w) puts its corner near
// the given frequency, and a = 1 passes the input straight through.
constexpr std::array<float, kToneSteps> DesignToneTable() {
    std::array<float, kToneSteps> table{};
    for (std::size_t i = 0; i + 1 < kToneSteps; ++i) {
        // e^(-w) as the fourth power of e^(-w/4), which keeps the series
        // argument under 0.35, where twelve terms are exact to double precision.
        const double w = 2.0 * std::numbers::pi * kToneCornersHz[i] / OutputControls::kSampleRate;
        const double quarter = constexpr_math::Exp(-w / 4.0);
        table[i] = static_cast<float>(1.0 - quarter * quarter * quarter * quarter);
    }
    table[kToneSteps - 1] = 1.0f;
    return table;
}

constexpr auto kToneTable = DesignToneTable();

// Maps a reading to a knob position in [0, 1], past the deadband.
float KnobPosition(float reading) { return std::clamp((reading - kDeadband) / (1.0f - 2.0f * kDeadband), 0.0f, 1.0f); }

// Cubing a linear pot's position approximates an audio taper: half way is
// about -18 dB, and fully down is silence.
float VolumeGain(float position) { return position * position * position; }

float Glide(float current, float target, float amount) {
    const float next = current + (target - current) * amount;
    return std::fabs(target - next) < kGlideSnap ? target : next;
}

// Interpolates the table at the tone position. A larger coefficient lets
// more treble through at every frequency, so turning the knob up never
// darkens the sound. At each entry's own position the weights are exactly
// 1 and 0, so fully open gives exactly 1.
float ToneCoefficient(float position) {
    const float index = position * static_cast<float>(kToneSteps - 1);
    const std::size_t low = std::min(static_cast<std::size_t>(index), kToneSteps - 2);
    const float high_weight = index - static_cast<float>(low);
    return kToneTable[low] * (1.0f - high_weight) + kToneTable[low + 1] * high_weight;
}
} // namespace

void OutputControls::SetKnobs(float volume, float tone) {
    volume_target_ = KnobPosition(volume);
    tone_target_ = KnobPosition(tone);
}

void OutputControls::Reset() {
    volume_ = volume_target_;
    tone_ = tone_target_;
    gain_ = VolumeGain(volume_);
    stage1_ = 0.0f;
    stage2_ = 0.0f;
}

void OutputControls::Process(float *samples, std::size_t frame_count) {
    if (frame_count == 0)
        return;

    // Glide the knob positions toward the latest readings.
    const float glide = std::min(1.0f, static_cast<float>(frame_count) * kGlidePerFrame);
    volume_ = Glide(volume_, volume_target_, glide);
    tone_ = Glide(tone_, tone_target_, glide);

    // Ramp the gain across the block from where the last block ended. With
    // the knob still, the step is exactly zero and the gain stays put.
    const float end_gain = VolumeGain(volume_);
    const float gain_step = (end_gain - gain_) / static_cast<float>(frame_count);
    float gain = gain_;

    // Low-pass through both stages, then scale. Written as a * x + (1 - a) * y
    // so that a = 1 gives exactly x.
    const float a = ToneCoefficient(tone_);
    const float hold = 1.0f - a;
    float stage1 = stage1_, stage2 = stage2_;
    for (std::size_t i = 0; i < frame_count; ++i) {
        stage1 = a * samples[i] + hold * stage1;
        stage2 = a * stage1 + hold * stage2;
        gain += gain_step;
        samples[i] = stage2 * gain;
    }

    stage1_ = stage1;
    stage2_ = stage2;
    gain_ = end_gain;
}
