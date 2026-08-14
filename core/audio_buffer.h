#pragma once

#include <cstddef>
#include <vector>

namespace audiofx::core {

// Simple mono float sample buffer used as the basic unit of DSP data.
class AudioBuffer {
public:
    explicit AudioBuffer(std::size_t numSamples = 0);

    void resize(std::size_t numSamples);
    void clear();

    std::size_t size() const noexcept;

    float* data() noexcept;
    const float* data() const noexcept;

    float& operator[](std::size_t index) noexcept;
    float operator[](std::size_t index) const noexcept;

private:
    std::vector<float> samples_;
};

} // namespace audiofx::core
