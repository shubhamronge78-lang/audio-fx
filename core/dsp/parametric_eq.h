#pragma once

#include <array>
#include <cstddef>

#include "biquad_coefficients.h"
#include "biquad_filter.h"

namespace audiofx::core::dsp {

// A parametric EQ made of a fixed maximum number of peaking/bell bands,
// each running through its own BiquadFilter in series.
//
// The band count is fixed at compile time (kMaxBands) so there is never
// any heap allocation on the audio thread: enabling/disabling a band or
// changing its parameters just rewrites a BiquadFilter's coefficients.
class ParametricEQ {
public:
    static constexpr std::size_t kMaxBands = 8;

    ParametricEQ();

    // Configures band `index` (0..kMaxBands-1) as an active peaking/bell
    // filter with the given parameters. Safe to call from a control
    // thread; does not allocate.
    void setBand(
        std::size_t index,
        float sampleRate, float freq, float q, float gainDb
    );

    // Disables band `index`, making it a no-op (identity) pass-through.
    void disableBand(std::size_t index);

    // Resets all bands' internal filter state (e.g. on playback stop/seek).
    void reset();

    // Runs `input` through all enabled bands in series.
    float process(float input) noexcept;

private:
    struct Band {
        BiquadFilter filter;
        bool enabled = false;
    };

    std::array<Band, kMaxBands> bands_;
};

} // namespace audiofx::core::dsp
