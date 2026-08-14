#include <cassert>
#include <chrono>
#include <thread>
#include <iostream>

#include "../linux/pipewire_backend.h"

using audiofx::pipewire::Backend;
using audiofx::pipewire::Config;

int main()
{
    Backend b;
    Config cfg;
    cfg.sampleRate = 48000;
    cfg.framesPerBlock = 128;

    assert(b.initialize(cfg));

    // set a passthrough realtime callback
    b.setRealtimeCallback([](const float* inFL, const float* inFR, float* outFL, float* outFR, uint32_t frames, void* userData){
        // transparent pass-through
        for (uint32_t i = 0; i < frames; ++i) {
            outFL[i] = inFL[i];
            outFR[i] = inFR[i];
        }
    }, nullptr);

    assert(b.start());

    // Wait up to 5 seconds for startup to reach usable state
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (b.nodeId() != 0 && b.portCount() == 4 && b.filterCreated() && b.isConnectedOrPaused()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Confirm required conditions
    assert(b.nodeId() != 0);
    assert(b.portCount() == 4);
    assert(b.filterCreated());
    assert(b.isConnectedOrPaused());

    // Print diagnostics and keep the backend alive for observation
    std::cout << "AudioFX nodeId=" << b.nodeId()
              << " portCount=" << b.portCount()
              << " connectedOrPaused=" << (b.isConnectedOrPaused() ? "true" : "false")
              << std::endl;

    // Interactive hold: wait until user presses Enter so you can inspect the node
    std::cout << "AudioFX is running. Press Enter to stop." << std::endl;
    std::cin.get();

    // After Enter pressed, stop and shutdown
    assert(b.stop());
    b.shutdown();

    return 0;
}
