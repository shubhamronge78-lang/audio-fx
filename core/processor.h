#pragma once

#include <cstdint>

namespace audiofx::core {

struct AudioBlock {
    // arrays of per-channel pointers (non-owning)
    float* const* inputs;
    float* const* outputs;
    uint32_t frames;
    uint32_t channels;
    uint32_t sampleRate;
};

class IProcessor {
public:
    virtual ~IProcessor() = default;

    // Configure called on control thread before realtime processing starts
    virtual void configure(uint32_t sampleRate, uint32_t channels, uint32_t maxFrames) noexcept = 0;

    virtual void start() noexcept = 0;
    virtual void stop() noexcept = 0;

    // Realtime-safe processing callback: no allocations, no locks, noexcept
    virtual void process(const AudioBlock& block) noexcept = 0;
};

} // namespace audiofx::core
