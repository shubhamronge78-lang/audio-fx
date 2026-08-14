#include <cassert>
#include <cmath>

#include "dsp/parametric_eq.h"

using audiofx::core::dsp::ParametricEQ;

namespace {

// Feeds a sine wave through the EQ and returns the steady-state peak
// amplitude (after discarding an initial settling period), so we can
// check the achieved gain in the time domain without exposing the
// EQ's internal coefficients.
float steadyStatePeak(ParametricEQ& eq, float freq, float sampleRate, int totalSamples, int settleSamples)
{
    float peak = 0.0f;
    for (int n = 0; n < totalSamples; ++n) {
        const float in = std::sin(2.0f * static_cast<float>(M_PI) * freq * n / sampleRate);
        const float out = eq.process(in);
        if (n >= settleSamples) {
            peak = std::max(peak, std::abs(out));
        }
    }
    return peak;
}

} // namespace

int main()
{
    const float sampleRate = 48000.0f;

    // No bands configured: EQ must be a transparent pass-through.
    {
        ParametricEQ eq;
        assert(std::abs(eq.process(1.0f) - 1.0f) < 1e-6f);
        assert(std::abs(eq.process(-0.5f) - (-0.5f)) < 1e-6f);
    }

    // A single band at 0 dB gain is also an identity pass-through.
    {
        ParametricEQ eq;
        eq.setBand(0, sampleRate, 1000.0f, 1.0f, 0.0f);
        assert(std::abs(eq.process(1.0f) - 1.0f) < 1e-4f);
        assert(std::abs(eq.process(0.3f) - 0.3f) < 1e-4f);
    }

    // disableBand should turn a configured band back into a no-op.
    {
        ParametricEQ eq;
        eq.setBand(0, sampleRate, 1000.0f, 1.0f, 12.0f); // strong boost
        eq.disableBand(0);
        assert(std::abs(eq.process(1.0f) - 1.0f) < 1e-6f);
        assert(std::abs(eq.process(-1.0f) - (-1.0f)) < 1e-6f);
    }

    // Single +6 dB band at 1 kHz: steady-state sine response at the
    // center frequency should approach the requested linear gain.
    {
        ParametricEQ eq;
        eq.setBand(0, sampleRate, 1000.0f, 1.0f, 6.0f);

        const float expectedGain = std::pow(10.0f, 6.0f / 20.0f);
        const float peak = steadyStatePeak(eq, 1000.0f, sampleRate, 4800, 2400);
        assert(std::abs(peak - expectedGain) < 0.02f);
    }

    // Two series bands, each +6 dB at the same frequency, should
    // roughly multiply in linear gain (~+12 dB total).
    {
        ParametricEQ eq;
        eq.setBand(0, sampleRate, 1000.0f, 1.0f, 6.0f);
        eq.setBand(1, sampleRate, 1000.0f, 1.0f, 6.0f);

        const float expectedGain = std::pow(10.0f, 6.0f / 20.0f) * std::pow(10.0f, 6.0f / 20.0f);
        const float peak = steadyStatePeak(eq, 1000.0f, sampleRate, 4800, 2400);
        assert(std::abs(peak - expectedGain) < 0.05f);
    }

    // reset() should clear filter state without changing configuration.
    {
        ParametricEQ eq;
        eq.setBand(0, sampleRate, 1000.0f, 1.0f, 6.0f);
        eq.process(1.0f);
        eq.process(0.5f);
        eq.reset();
        // Right after reset, with zeroed history, a 0 input must give 0 output.
        assert(std::abs(eq.process(0.0f)) < 1e-6f);
    }

    return 0;
}
