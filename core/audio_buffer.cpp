#include "audio_buffer.h"

#include <algorithm>

namespace audiofx::core {

AudioBuffer::AudioBuffer(std::size_t numSamples)
    : samples_(numSamples, 0.0f)
{
}

void AudioBuffer::resize(std::size_t numSamples)
{
    samples_.resize(numSamples, 0.0f);
}

void AudioBuffer::clear()
{
    std::fill(samples_.begin(), samples_.end(), 0.0f);
}

std::size_t AudioBuffer::size() const noexcept
{
    return samples_.size();
}

float* AudioBuffer::data() noexcept
{
    return samples_.data();
}

const float* AudioBuffer::data() const noexcept
{
    return samples_.data();
}

float& AudioBuffer::operator[](std::size_t index) noexcept
{
    return samples_[index];
}

float AudioBuffer::operator[](std::size_t index) const noexcept
{
    return samples_[index];
}

} // namespace audiofx::core
