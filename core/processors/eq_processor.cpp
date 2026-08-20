#include "eq_processor.h"

namespace audiofx::core::processors {

void EQProcessor::configure(
    uint32_t sampleRate,
    uint32_t channels,
    uint32_t maxFrames
) noexcept
{
    sampleRate_ = sampleRate;
    channels_ = channels;
    maxFrames_ = maxFrames;
}

void EQProcessor::start() noexcept
{
    // Reset filter state for both channels
    eqLeft_.reset();
    eqRight_.reset();
}

void EQProcessor::stop() noexcept
{
    // No-op: no resources to release, no blocking operations
}

void EQProcessor::process(const AudioBlock& block) noexcept
{
    // Handle stereo: process each channel independently
    if (block.channels >= 2) {
        float* inL = block.inputs[0];
        float* inR = block.inputs[1];
        float* outL = block.outputs[0];
        float* outR = block.outputs[1];

        for (uint32_t f = 0; f < block.frames; ++f) {
            outL[f] = eqLeft_.process(inL[f]);
            outR[f] = eqRight_.process(inR[f]);
        }
    }
    // Handle mono: process single channel
    else if (block.channels == 1) {
        float* in = block.inputs[0];
        float* out = block.outputs[0];

        for (uint32_t f = 0; f < block.frames; ++f) {
            out[f] = eqLeft_.process(in[f]);
        }
    }
    // Handle unsupported channel count: pass-through
    else {
        for (uint32_t c = 0; c < block.channels; ++c) {
            float* in = block.inputs[c];
            float* out = block.outputs[c];
            for (uint32_t f = 0; f < block.frames; ++f) {
                out[f] = in[f];
            }
        }
    }
}

void EQProcessor::setPeakingBand(
    std::size_t band,
    float frequency,
    float q,
    float gainDb
) noexcept
{
    eqLeft_.setBand(band, sampleRate_, frequency, q, gainDb);
    eqRight_.setBand(band, sampleRate_, frequency, q, gainDb);
}

void EQProcessor::disableBand(std::size_t band) noexcept
{
    eqLeft_.disableBand(band);
    eqRight_.disableBand(band);
}

} // namespace audiofx::core::processors
