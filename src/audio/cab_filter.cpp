#include "audio/cab_filter.h"
#include "audio/constexpr_math.h"
#include <numbers>

namespace {
enum class Shape { HighPass, LowPass, Peak };

struct StageSpec {
    Shape shape;
    double frequency_hz;
    double q;
    double gain_db;
};

// Roughly a closed-back 12-inch speaker. The two low-pass stages share a
// corner and their Qs make a 4th-order Butterworth (24 dB/octave).
// Columns: shape, corner or center frequency (Hz), Q, gain (dB).
// clang-format off
constexpr StageSpec kStageSpecs[] = {
    {Shape::HighPass,   75.0, 0.707,  0.0}, // Below the speaker's useful range.
    {Shape::Peak,      110.0, 1.4,    3.0}, // Cone resonance.
    {Shape::Peak,      400.0, 1.0,   -3.0}, // Low-mid scoop.
    {Shape::Peak,     2500.0, 1.5,    2.5}, // Presence.
    {Shape::LowPass,  5000.0, 0.541,  0.0},
    {Shape::LowPass,  5000.0, 1.307,  0.0},
};
// clang-format on
static_assert(sizeof(kStageSpecs) / sizeof(kStageSpecs[0]) == CabFilter::kStages);

// The design uses compile-time Taylor series instead of libm. Twelve terms
// are exact to double precision for the angles and gains above.
using constexpr_math::Cos;
using constexpr_math::Exp;
using constexpr_math::Sin;

constexpr CabFilter::Coefficients Design(const StageSpec &spec) {
    // Bilinear-transform terms from the Audio EQ Cookbook (R. Bristow-Johnson).
    const double w0 = 2.0 * std::numbers::pi * spec.frequency_hz / CabFilter::kSampleRate;
    const double cos_w0 = Cos(w0);
    const double alpha = Sin(w0) / (2.0 * spec.q);
    const double amplitude = Exp(spec.gain_db / 40.0 * std::numbers::ln10);

    // Unnormalized coefficients for this stage's shape.
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;
    switch (spec.shape) {
    case Shape::HighPass:
        b0 = (1.0 + cos_w0) / 2.0;
        b1 = -(1.0 + cos_w0);
        b2 = b0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cos_w0;
        a2 = 1.0 - alpha;
        break;
    case Shape::LowPass:
        b0 = (1.0 - cos_w0) / 2.0;
        b1 = 1.0 - cos_w0;
        b2 = b0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cos_w0;
        a2 = 1.0 - alpha;
        break;
    case Shape::Peak:
        b0 = 1.0 + alpha * amplitude;
        b1 = -2.0 * cos_w0;
        b2 = 1.0 - alpha * amplitude;
        a0 = 1.0 + alpha / amplitude;
        a1 = -2.0 * cos_w0;
        a2 = 1.0 - alpha / amplitude;
        break;
    }

    // Normalize by a0 so Process needs no division.
    return {static_cast<float>(b0 / a0), static_cast<float>(b1 / a0), static_cast<float>(b2 / a0), static_cast<float>(a1 / a0), static_cast<float>(a2 / a0)};
}

constexpr std::array<CabFilter::Coefficients, CabFilter::kStages> DesignAll() {
    std::array<CabFilter::Coefficients, CabFilter::kStages> stages{};
    for (std::size_t i = 0; i < CabFilter::kStages; ++i)
        stages[i] = Design(kStageSpecs[i]);
    return stages;
}

constexpr auto kCoefficients = DesignAll();
} // namespace

void CabFilter::Reset() {
    for (State &state : state_)
        state = {};
}

void CabFilter::Process(float *samples, std::size_t frame_count) {
    for (std::size_t s = 0; s < kStages; ++s) {
        // One stage at a time over the whole block, keeping its coefficients
        // and state in registers for the inner loop.
        const Coefficients &c = kCoefficients[s];
        float z1 = state_[s].z1, z2 = state_[s].z2;

        // Transposed direct form II: one output, then the two state updates.
        for (std::size_t i = 0; i < frame_count; ++i) {
            const float x = samples[i];
            const float y = c.b0 * x + z1;
            z1 = c.b1 * x - c.a1 * y + z2;
            z2 = c.b2 * x - c.a2 * y;
            samples[i] = y;
        }

        state_[s].z1 = z1;
        state_[s].z2 = z2;
    }
}
