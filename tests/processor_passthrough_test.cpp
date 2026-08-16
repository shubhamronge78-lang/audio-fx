#include <cassert>
#include <cstring>
#include <iostream>

#include "../core/audio_buffer.h"
#include "../core/processor.h"

using namespace audiofx::core;

int main()
{
    const uint32_t sampleRate = 48000;
    const uint32_t frames = 128;
    const uint32_t channels = 1;

    AudioBuffer inBuf(frames);
    AudioBuffer outBuf(frames);

    // fill input with a pattern
    for (uint32_t i = 0; i < frames; ++i) inBuf[i] = (float)i / frames;

    // prepare pointer arrays
    float* inputsArr[1] = { inBuf.data() };
    float* outputsArr[1] = { outBuf.data() };

    // simple pass-through processor implemented inline
    struct PassThrough : IProcessor {
        void configure(uint32_t, uint32_t, uint32_t) noexcept override {}
        void start() noexcept override {}
        void stop() noexcept override {}
        void process(const AudioBlock& block) noexcept override {
            // copy per-channel
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

    // verify output equals input
    for (uint32_t i = 0; i < frames; ++i) {
        assert(outBuf[i] == inBuf[i]);
    }

    std::cout << "processor_passthrough_test: PASS\n";
    return 0;
}
