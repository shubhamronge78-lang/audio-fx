#include <cassert>
#include <atomic>
#include <chrono>
#include <thread>

#include "../linux/pipewire_backend.h"
#include "../core/processor.h"

using audiofx::pipewire::Backend;
using audiofx::pipewire::Config;
using audiofx::core::IProcessor;

struct TestProcessor final : IProcessor {
    std::atomic<uint32_t> configureCount{0};
    std::atomic<uint32_t> startCount{0};
    std::atomic<uint32_t> stopCount{0};
    std::atomic<uint64_t> processCount{0};

    void configure(
        uint32_t,
        uint32_t,
        uint32_t
    ) noexcept override
    {
        configureCount.fetch_add(1, std::memory_order_relaxed);
    }

    void start() noexcept override
    {
        startCount.fetch_add(1, std::memory_order_relaxed);
    }

    void stop() noexcept override
    {
        stopCount.fetch_add(1, std::memory_order_relaxed);
    }

    void process(
        const audiofx::core::AudioBlock& block
    ) noexcept override
    {
        processCount.fetch_add(1, std::memory_order_relaxed);

        for (uint32_t c = 0; c < block.channels; ++c) {
            float* in = block.inputs[c];
            float* out = block.outputs[c];

            for (uint32_t f = 0; f < block.frames; ++f)
                out[f] = in[f];
        }
    }
};

int main()
{
    Backend backend;

    Config cfg;
    cfg.sampleRate = 48000;
    cfg.framesPerBlock = 128;

    assert(backend.initialize(cfg));

    TestProcessor procA;
    TestProcessor procB;

    /*
     * Install A while stopped.
     */
    backend.setProcessor(&procA);

    assert(procA.configureCount.load(std::memory_order_relaxed) == 1);
    assert(procA.startCount.load(std::memory_order_relaxed) == 0);
    assert(procA.stopCount.load(std::memory_order_relaxed) == 0);

    /*
     * Start the backend.
     *
     * PipeWire processing is intentionally not required here. The
     * existing PipeWireProcessorIntegrationTest already verifies
     * realtime invocation. This test verifies processor lifecycle
     * semantics while the backend is running.
     */
    assert(backend.start());

    assert(backend.isRunning());

    assert(procA.configureCount.load(std::memory_order_relaxed) == 1);
    assert(procA.startCount.load(std::memory_order_relaxed) == 1);
    assert(procA.stopCount.load(std::memory_order_relaxed) == 0);

    /*
     * Regression case:
     *
     * The public contract says replacing/removing a processor while
     * realtime processing is active is unsupported.
     *
     * Therefore setProcessor() while running must NOT configure,
     * publish, or start procB.
     */
    backend.setProcessor(&procB);

    assert(
        procB.configureCount.load(std::memory_order_relaxed) == 0
    );

    assert(
        procB.startCount.load(std::memory_order_relaxed) == 0
    );

    assert(
        procB.stopCount.load(std::memory_order_relaxed) == 0
    );

    /*
     * A must remain the active processor.
     */
    assert(
        procA.configureCount.load(std::memory_order_relaxed) == 1
    );

    assert(
        procA.startCount.load(std::memory_order_relaxed) == 1
    );

    /*
     * Shutdown must stop A exactly once.
     */
    backend.stop();

    assert(
        procA.stopCount.load(std::memory_order_relaxed) == 1
    );

    assert(
        procB.stopCount.load(std::memory_order_relaxed) == 0
    );

    backend.shutdown();

    return 0;
}
