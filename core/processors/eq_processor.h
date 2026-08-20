#pragma once

#include <cstdint>

#include "../processor.h"
#include "../dsp/parametric_eq.h"

namespace audiofx::core::processors {

// EQProcessor: A realtime-safe parametric EQ processor.
//
// Implements IProcessor to process stereo audio through two independent
// ParametricEQ instances (one per channel) to maintain independent filter state.
//
// Safe for use in realtime audio threads: no allocations, no locks,
// no blocking operations in process().
class EQProcessor final : public IProcessor {
public:
    EQProcessor() = default;
    ~EQProcessor() = default;

    // Configure the processor with the negotiated audio parameters.
    // Called on the control thread before start().
    void configure(
        uint32_t sampleRate,
        uint32_t channels,
        uint32_t maxFrames
    ) noexcept override;

    // Start audio processing. Resets filter state.
    // Called on the control thread before process() begins.
    void start() noexcept override;

    // Stop audio processing.
    // Called on the control thread after process() stops.
    void stop() noexcept override;

    // Realtime-safe audio processing callback.
    // Processes stereo channels independently through their respective EQ instances.
    // Called from the realtime audio thread.
    void process(const AudioBlock& block) noexcept override;

    // Control thread: Configure a peaking/bell EQ band.
    // Safe to call only while the backend is stopped (before start()).
    // Uses the sample rate from configure().
    void setPeakingBand(
        std::size_t band,
        float frequency,
        float q,
        float gainDb
    ) noexcept;

    // Control thread: Disable an EQ band.
    // Safe to call only while the backend is stopped (before start()).
    void disableBand(std::size_t band) noexcept;

private:
    // Dual-channel EQ state. Each channel maintains independent filter state
    // to prevent cross-channel interference in the IIR filter history.
    dsp::ParametricEQ eqLeft_;
    dsp::ParametricEQ eqRight_;

    // Configuration parameters (cached from configure())
    uint32_t sampleRate_ = 48000;
    uint32_t channels_ = 0;
    uint32_t maxFrames_ = 0;
};

} // namespace audiofx::core::processors
