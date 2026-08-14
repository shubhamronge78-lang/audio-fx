#include <cassert>
#include <chrono>
#include <thread>

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

    assert(b.start());

    // Wait up to 3 seconds for nodeId/ports to appear
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (b.nodeId() != 0 && b.portCount() == 4 && b.filterCreated() && b.isConnectedOrPaused())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Milestone checks
    assert(b.nodeId() != 0);
    assert(b.portCount() == 4);
    assert(b.filterCreated());
    assert(b.isConnectedOrPaused());

    assert(b.stop());
    b.shutdown();

    return 0;
}
