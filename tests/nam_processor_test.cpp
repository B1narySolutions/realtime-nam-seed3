#include "audio/nam_processor.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <new>

// Passes audio through and records how it was prepared.
class FakeModel : public AmpModel {
  public:
    explicit FakeModel(double sample_rate_hz = 48000.0) : sample_rate_hz_(sample_rate_hz) {}
    double SampleRate() const override { return sample_rate_hz_; }
    void Reset(int max_block_size) override {
        ++resets;
        reset_block_size = max_block_size;
    }
    void Process(const float *input, float *output, int frames) override { std::copy(input, input + frames, output); }
    int resets = 0;
    int reset_block_size = 0;

  private:
    double sample_rate_hz_;
};

class FailingModel : public FakeModel {
  public:
    void Reset(int) override { throw std::bad_alloc(); }
};

static std::unique_ptr<AmpModel> MakeModel() { return std::make_unique<FakeModel>(); }

int main() {
    NamProcessor processor;
    std::array<float, 48> input{}, output{};
    output.fill(123.0f);
    assert(!processor.Process(input.data(), output.data(), 48));
    assert(output[0] == 123.0f);
    assert(!processor.SetModel(nullptr, 48000.0, 48));
    assert(!processor.SetModel(MakeModel(), 44100.0, 48));
    assert(!processor.SetModel(MakeModel(), 48000.0, 0));
    assert(processor.SetModel(MakeModel(), 48000.0, 48));
    assert(!processor.SetModel(MakeModel(), std::numeric_limits<double>::quiet_NaN(), 48));
    assert(!processor.SetModel(std::make_unique<FakeModel>(-1.0), 48000.0, 48));
    auto resetting = std::make_unique<FakeModel>();
    auto *reset = resetting.get();
    assert(processor.SetModel(std::move(resetting), 48000.0, 48));
    assert(reset->resets == 1 && reset->reset_block_size == 48); // SetModel must reset exactly once.
    // A failed replacement keeps the previous model and block size.
    assert(!processor.SetModel(std::make_unique<FailingModel>(), 48000.0, 96));
    assert(!processor.Process(input.data(), output.data(), 49));
    assert(!processor.Process(nullptr, output.data(), 48));
    assert(!processor.Process(input.data(), input.data(), 48));
    assert(output[0] == 123.0f);
    for (int block = 0; block < 10; ++block) {
        const int frames = block % 2 ? 17 : 48;
        for (int i = 0; i < frames; ++i)
            input[i] = std::sin(static_cast<float>(block * 48 + i) * 0.1f);
        assert(processor.Process(input.data(), output.data(), frames));
        for (int i = 0; i < frames; ++i)
            assert(output[i] == input[i]);
    }
}
