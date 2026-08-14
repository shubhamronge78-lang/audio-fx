#pragma once

namespace audiofx::core::dsp {

// Normalized biquad coefficients (already divided by a0), ready to
// hand straight to BiquadFilter::setCoefficients().
struct BiquadCoefficients {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
};

// RBJ Audio EQ Cookbook peaking/bell filter.
//
// sampleRate: in Hz, must be > 0.
// freq:       center frequency in Hz, should be < sampleRate / 2.
// q:          quality factor, must be > 0. Higher Q = narrower bell.
// gainDb:     boost/cut at the center frequency, in dB. 0 dB = no-op.
BiquadCoefficients makePeakingEq(
    float sampleRate, float freq, float q, float gainDb
);

} // namespace audiofx::core::dsp
