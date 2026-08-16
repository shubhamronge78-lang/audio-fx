#pragma once

#include <cstdint>
#include <cstddef>
// include core processor interface
#include "../core/processor.h"

namespace audiofx::pipewire {

struct Impl; // forward declaration for Pimpl (defined in .cpp)

struct Config {
    uint32_t sampleRate = 48000;
    uint32_t framesPerBlock = 128;
};

class Backend {
public:
    using RtCallback = void(*)(const float* inFL, const float* inFR,
                               float* outFL, float* outFR,
                               uint32_t frames, void* userData);

    Backend();
    ~Backend();

    // Prepare resources but do not start the control/main loop or connect filter
    bool initialize(const Config& cfg);

    // Connect filter and start control/main-loop processing
    bool start();

    // Stop processing and disconnect filter
    bool stop();

    // Destroy resources
    void shutdown();

    bool isRunning() const;

    // Testable inspection helpers (integration tests will use these)
    uint32_t nodeId() const; // 0 == invalid
    std::size_t portCount() const;
    bool filterCreated() const;
    bool isConnectedOrPaused() const;

    void setRealtimeCallback(RtCallback cb, void* userData);

    // Install a non-owning processor to be invoked from the realtime
    // `filter_process()` callback. The processor must outlive the backend
    // and must obey realtime safety rules (no allocations, no locks).
    void setProcessor(core::IProcessor* processor, void* userData = nullptr);
    // Note: production code does not expose realtime diagnostics.

private:
    // Pimpl not required for the first milestone; implementation
    // details are in the .cpp file.
    Impl* impl_ = nullptr;
};

} // namespace audiofx::pipewire
