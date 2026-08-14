#include "biquad_filter.h"

namespace audiofx::core::dsp {

BiquadFilter::BiquadFilter() = default;

void BiquadFilter::setCoefficients(
    float b0, float b1, float b2,
    float a1, float a2
)
{
    b0_ = b0;
    b1_ = b1;
    b2_ = b2;
    a1_ = a1;
    a2_ = a2;
}

void BiquadFilter::reset()
{
    x1_ = 0.0f;
    x2_ = 0.0f;
    y1_ = 0.0f;
    y2_ = 0.0f;
}

float BiquadFilter::process(float input) noexcept
{
    const float output =
        b0_ * input +
        b1_ * x1_ +
        b2_ * x2_ -
        a1_ * y1_ -
        a2_ * y2_;

    x2_ = x1_;
    x1_ = input;

    y2_ = y1_;
    y1_ = output;

    return output;
}

} // namespace audiofx::core::dsp
