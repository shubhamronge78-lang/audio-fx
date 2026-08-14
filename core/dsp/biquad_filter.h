#pragma once

namespace audiofx::core::dsp {

// A standard biquad (2nd-order) IIR filter, direct form I.
// Coefficients are set externally (e.g. by ParametricEQ) via setCoefficients().
class BiquadFilter {
public:
    BiquadFilter();

    void setCoefficients(
        float b0, float b1, float b2,
        float a1, float a2
    );

    void reset();

    float process(float input) noexcept;

private:
    float b0_ = 1.0f;
    float b1_ = 0.0f;
    float b2_ = 0.0f;
    float a1_ = 0.0f;
    float a2_ = 0.0f;

    float x1_ = 0.0f;
    float x2_ = 0.0f;
    float y1_ = 0.0f;
    float y2_ = 0.0f;
};

} // namespace audiofx::core::dsp
