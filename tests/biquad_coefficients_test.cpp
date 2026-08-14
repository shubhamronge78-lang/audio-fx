#include <cassert>
#include <cmath>
#include <complex>

#include "dsp/biquad_coefficients.h"

using audiofx::core::dsp::BiquadCoefficients;
using audiofx::core::dsp::makePeakingEq;

namespace {

float magnitudeAt(const BiquadCoefficients& c, float freq, float sampleRate)
{
    const float w = 2.0f * static_cast<float>(M_PI) * freq / sampleRate;
    const std::complex<float> zInv(std::cos(w), -std::sin(w)); // e^{-jw}

    const std::complex<float> numerator =
        c.b0 + c.b1 * zInv + c.b2 * zInv * zInv;
    const std::complex<float> denominator =
        1.0f + c.a1 * zInv + c.a2 * zInv * zInv;

    return std::abs(numerator / denominator);
}

} // namespace

int main()
{
    const float sampleRate = 48000.0f;

    // 0 dB gain must be an exact identity filter regardless of freq/Q.
    {
        const BiquadCoefficients c = makePeakingEq(sampleRate, 1000.0f, 1.0f, 0.0f);
        assert(std::abs(magnitudeAt(c, 100.0f, sampleRate) - 1.0f) < 1e-4f);
        assert(std::abs(magnitudeAt(c, 1000.0f, sampleRate) - 1.0f) < 1e-4f);
        assert(std::abs(magnitudeAt(c, 5000.0f, sampleRate) - 1.0f) < 1e-4f);
    }

    // +6 dB boost at 1 kHz, Q=1: magnitude at center should match requested gain.
    {
        const BiquadCoefficients c = makePeakingEq(sampleRate, 1000.0f, 1.0f, 6.0f);
        const float expectedGain = std::pow(10.0f, 6.0f / 20.0f);
        assert(std::abs(magnitudeAt(c, 1000.0f, sampleRate) - expectedGain) < 1e-3f);

        // Far from center, the bell should have decayed back to unity.
        assert(std::abs(magnitudeAt(c, 50.0f, sampleRate) - 1.0f) < 0.05f);
    }

    // -6 dB cut at 1 kHz, Q=1.
    {
        const BiquadCoefficients c = makePeakingEq(sampleRate, 1000.0f, 1.0f, -6.0f);
        const float expectedGain = std::pow(10.0f, -6.0f / 20.0f);
        assert(std::abs(magnitudeAt(c, 1000.0f, sampleRate) - expectedGain) < 1e-3f);
    }

    return 0;
}
