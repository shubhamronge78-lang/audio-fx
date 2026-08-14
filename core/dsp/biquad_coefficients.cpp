#include "biquad_coefficients.h"

#include <cmath>

namespace audiofx::core::dsp {

BiquadCoefficients makePeakingEq(
    float sampleRate, float freq, float q, float gainDb
)
{
    const float A = std::pow(10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * static_cast<float>(M_PI) * freq / sampleRate;
    const float cosW0 = std::cos(w0);
    const float sinW0 = std::sin(w0);
    const float alpha = sinW0 / (2.0f * q);

    const float b0 = 1.0f + alpha * A;
    const float b1 = -2.0f * cosW0;
    const float b2 = 1.0f - alpha * A;
    const float a0 = 1.0f + alpha / A;
    const float a1 = -2.0f * cosW0;
    const float a2 = 1.0f - alpha / A;

    BiquadCoefficients coeffs;
    coeffs.b0 = b0 / a0;
    coeffs.b1 = b1 / a0;
    coeffs.b2 = b2 / a0;
    coeffs.a1 = a1 / a0;
    coeffs.a2 = a2 / a0;
    return coeffs;
}

} // namespace audiofx::core::dsp
