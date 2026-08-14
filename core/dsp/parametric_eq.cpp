#include "parametric_eq.h"

#include <cassert>

namespace audiofx::core::dsp {

ParametricEQ::ParametricEQ() = default;

void ParametricEQ::setBand(
    std::size_t index,
    float sampleRate, float freq, float q, float gainDb
)
{
    assert(index < kMaxBands);

    const BiquadCoefficients coeffs = makePeakingEq(sampleRate, freq, q, gainDb);
    Band& band = bands_[index];
    band.filter.setCoefficients(coeffs.b0, coeffs.b1, coeffs.b2, coeffs.a1, coeffs.a2);
    band.enabled = true;
}

void ParametricEQ::disableBand(std::size_t index)
{
    assert(index < kMaxBands);
    bands_[index].enabled = false;
}

void ParametricEQ::reset()
{
    for (Band& band : bands_) {
        band.filter.reset();
    }
}

float ParametricEQ::process(float input) noexcept
{
    float output = input;
    for (Band& band : bands_) {
        if (band.enabled) {
            output = band.filter.process(output);
        }
    }
    return output;
}

} // namespace audiofx::core::dsp
