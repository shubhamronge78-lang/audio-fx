#include <cassert>
#include <cstring>
#include <iostream>

#include "../core/audio_buffer.h"
#include "../core/processor.h"

using namespace audiofx::core;

int main()
{
    const uint32_t sampleRate = 48000;
    const uint32_t frames = 257; // non-power-of-two to test counts
    const uint32_t channels = 2;

    AudioBuffer inL(frames);
    AudioBuffer inR(frames);
    AudioBuffer outL(frames);
    AudioBuffer outR(frames);

    for (uint32_t i = 0; i < frames; ++i) {
        inL[i] = (float)i / frames;
        inR[i] = (float)(frames - i) / frames;
    }

    float* inputsArr[2] = { inL.data(), inR.data() };
    float* outputsArr[2] = { outL.data(), outR.data() };

    struct StereoPass : IProcessor {
        void configure(uint32_t, uint32_t, uint32_t) noexcept override {}
        void start() noexcept override {}
        void stop() noexcept override {}
        void process(const AudioBlock& block) noexcept override {
            for (uint32_t c = 0; c < block.channels; ++c) {
                float* in = block.inputs[c];
                float* out = block.outputs[c];
                for (uint32_t f = 0; f < block.frames; ++f) out[f] = in[f];
            }
        }
    } proc;

    proc.configure(sampleRate, channels, frames);
    proc.start();

    AudioBlock blk { inputsArr, outputsArr, frames, channels, sampleRate };
    proc.process(blk);

    proc.stop();

    for (uint32_t i = 0; i < frames; ++i) {
        assert(outL[i] == inL[i]);
        assert(outR[i] == inR[i]);
    }

    std::cout << "processor_stereo_test: PASS\n";
    return 0;
}
