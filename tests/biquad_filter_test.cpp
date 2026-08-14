#include <cassert>
#include <cmath>

#include "dsp/biquad_filter.h"

using audiofx::core::dsp::BiquadFilter;

int main()
{
    BiquadFilter filter;

    // Identity coefficients: output should equal input.
    filter.setCoefficients(1.0f, 0.0f, 0.0f, 0.0f, 0.0f);

    assert(std::abs(filter.process(1.0f) - 1.0f) < 1e-6f);
    assert(std::abs(filter.process(0.5f) - 0.5f) < 1e-6f);
    assert(std::abs(filter.process(-1.0f) - (-1.0f)) < 1e-6f);

    // Reset should clear internal state.
    filter.reset();

    // A simple gain filter (b0=2) should double the input.
    filter.setCoefficients(2.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    assert(std::abs(filter.process(1.0f) - 2.0f) < 1e-6f);

    return 0;
}
