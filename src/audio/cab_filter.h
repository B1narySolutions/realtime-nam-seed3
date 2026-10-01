#pragma once

#include <array>
#include <cstddef>

// A fixed EQ that stands in for a guitar speaker cabinet, so the amp-only
// models don't sound fizzy through headphones or a PA. It cuts rumble below
// the speaker's resonance, adds that resonance and some presence, scoops the
// low mids, and rolls off steeply above 5 kHz, where real speakers stop.
//
// A cascade of biquads rather than a measured cabinet impulse response: it
// costs a few microseconds per block and needs no licensed IR data.
class CabFilter {
  public:
    // The coefficients are designed at compile time for this rate only.
    static constexpr double kSampleRate = 48000.0;
    static constexpr std::size_t kStages = 6;

    // Normalized biquad coefficients (a0 = 1).
    struct Coefficients {
        float b0, b1, b2, a1, a2;
    };

    // Clears the filter's memory so the next sample starts from silence.
    void Reset();

    // Filters samples in place. Never allocates, and the result does not
    // depend on how the stream is split into blocks.
    void Process(float *samples, std::size_t frame_count);

  private:
    // Transposed direct form II state, one pair per stage.
    struct State {
        float z1 = 0.0f;
        float z2 = 0.0f;
    };

    std::array<State, kStages> state_{};
};
