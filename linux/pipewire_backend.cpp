#include "pipewire_backend.h"

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <pipewire/keys.h>

#include <spa/buffer/buffer.h>

#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <string>
// diagnostics
#include <cstdio>
#include <cerrno>
#include <cstring>

namespace audiofx::pipewire {

struct Impl {
    Config cfg;
    std::atomic<bool> initialized{false};
    std::atomic<bool> running{false};
    std::atomic<uint32_t> nodeId{0};
    std::atomic<std::size_t> portCount{0};
    std::atomic<bool> filterCreated{false};
    std::atomic<bool> connectedOrPaused{false};
    std::atomic<bool> startFailed{false};

    Backend::RtCallback rtCb = nullptr;
    void* rtUser = nullptr;

    struct pw_main_loop *main_loop = nullptr;
    struct pw_loop *loop = nullptr;
    struct pw_context *context = nullptr;
    struct pw_core *core = nullptr;
    struct pw_filter *filter = nullptr;

    std::thread controlThread;
    std::mutex mtx;
    std::condition_variable cv;
    bool threadShouldExit = false;

    std::vector<void*> portData;
    std::vector<std::string> portNames;
    // Processor integration.
    // Published only while the backend is stopped, so the realtime thread
    // never races with setProcessor().
    std::atomic<audiofx::core::IProcessor*> processor{nullptr};
    // Fixed per-channel pointer arrays for realtime use (planar)
    float* inputsPtrs[2] = {nullptr, nullptr};
    float* outputsPtrs[2] = {nullptr, nullptr};
};

Backend::Backend()
    : impl_(new Impl{})
{}

Backend::~Backend()
{
    shutdown();
    delete impl_;
    impl_ = nullptr;
}

static void filter_state_changed(void *data, enum pw_filter_state old, enum pw_filter_state state, const char *error)
{
    Impl *impl = static_cast<Impl*>(data);
    if (state == PW_FILTER_STATE_PAUSED || state == PW_FILTER_STATE_STREAMING) {
        impl->connectedOrPaused.store(true);
    }
}

static void filter_process(void *data, struct spa_io_position *position)
{
    Impl *impl = static_cast<Impl*>(data);

    // Realtime callback: use PipeWire DSP helper to get per-port buffers.
    if (!impl->filter)
        return;

    const size_t expected = 4;
    if (impl->portData.size() < expected)
        return;

    // Use position->clock.duration as the frame/sample count
    if (!position) return;
    uint32_t frames = position->clock.duration;
    if (frames == 0) return;

    float* inFL = static_cast<float*>(pw_filter_get_dsp_buffer(impl->portData[0], frames));
    float* inFR = static_cast<float*>(pw_filter_get_dsp_buffer(impl->portData[1], frames));
    float* outFL = static_cast<float*>(pw_filter_get_dsp_buffer(impl->portData[2], frames));
    float* outFR = static_cast<float*>(pw_filter_get_dsp_buffer(impl->portData[3], frames));

    if (!inFL || !inFR || !outFL || !outFR)
        return;

    // Prepare planar pointer arrays (no allocations)
    impl->inputsPtrs[0] = inFL;
    impl->inputsPtrs[1] = inFR;
    impl->outputsPtrs[0] = outFL;
    impl->outputsPtrs[1] = outFR;

    // If a processor is installed, call it directly in the realtime
    // callback using the platform-neutral AudioBlock interface.
    auto* processor = impl->processor.load(std::memory_order_acquire);

    if (processor) {
        audiofx::core::AudioBlock blk {
            /*inputs*/ impl->inputsPtrs,
            /*outputs*/ impl->outputsPtrs,
            frames,
            /*channels*/ 2,
            impl->cfg.sampleRate
        };
        processor->process(blk);
    } else if (impl->rtCb) {
        impl->rtCb(inFL, inFR, outFL, outFR, frames, impl->rtUser);
    } else {
        // Transparent pass-through
        for (uint32_t i = 0; i < frames; ++i) {
            outFL[i] = inFL[i];
            outFR[i] = inFR[i];
        }
    }
}

static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    nullptr, /* destroy */
    filter_state_changed,
    nullptr, /* io_changed */
    nullptr, /* param_changed */
    nullptr, /* add_buffer */
    nullptr, /* remove_buffer */
    filter_process,
    nullptr, /* drained */
    nullptr  /* command */
};

bool Backend::initialize(const Config& cfg)
{
    if (impl_->initialized.load())
        return true;

    impl_->cfg = cfg;

    // Initialize PipeWire library (no main-loop started yet)
    pw_init(nullptr, nullptr);

    impl_->main_loop = pw_main_loop_new(nullptr);
    if (!impl_->main_loop)
        return false;

    impl_->loop = pw_main_loop_get_loop(impl_->main_loop);
    if (!impl_->loop) {
        pw_main_loop_destroy(impl_->main_loop);
        impl_->main_loop = nullptr;
        return false;
    }

    // Create context and core will be created when connecting the filter
    impl_->context = pw_context_new(pw_main_loop_get_loop(impl_->main_loop), nullptr, 0);

    impl_->initialized.store(true);
    return true;
}

bool Backend::start()
{
    if (!impl_->initialized.load())
        return false;

    if (impl_->running.load())
        return true;

    // Start control/main-loop thread
    impl_->threadShouldExit = false;
    impl_->startFailed.store(false);
    impl_->controlThread = std::thread([this]() {
        Impl *impl = this->impl_;

        // Create filter properties
        struct pw_properties *props = pw_properties_new(
            PW_KEY_APP_NAME, "AudioFX",
            PW_KEY_NODE_NAME, "AudioFX",
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Filter",
            PW_KEY_MEDIA_ROLE, "DSP",
            PW_KEY_NODE_DESCRIPTION, "AudioFX DSP Filter",
            NULL);

        // Create core by connecting the context to the default PipeWire server
        impl->core = pw_context_connect(impl->context, nullptr, 0);
        if (!impl->core) {
            int err = errno;
            fprintf(stderr, "pw_context_connect() failed: %s (%d)\n", strerror(err), err);
            impl->startFailed.store(true);
            impl->cv.notify_all();
            return;
        }

        // Create filter (use pw_loop, not pw_core)
        impl->filter = pw_filter_new_simple(impl->loop, "AudioFX", props, &filter_events, impl);
        if (!impl->filter) {
            int err = errno;
            fprintf(stderr, "pw_filter_new_simple() failed: %s (%d)\n", strerror(err), err);
            impl->startFailed.store(true);
            impl->cv.notify_all();
            return;
        }
        const char* port_names[4] = {"input_FL","input_FR","output_FL","output_FR"};
        const enum pw_direction dirs[4] = {PW_DIRECTION_INPUT, PW_DIRECTION_INPUT, PW_DIRECTION_OUTPUT, PW_DIRECTION_OUTPUT};
        const char* channels[4] = {"FL","FR","FL","FR"};

        impl->portData.clear();
        impl->portNames.clear();
        for (int i = 0; i < 4; ++i) {
            struct pw_properties *pp = pw_properties_new(
                PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                PW_KEY_AUDIO_CHANNEL, channels[i],
                NULL);

            void *port_data = pw_filter_add_port(impl->filter, dirs[i], PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(void*), pp, nullptr, 0);
            impl->portData.push_back(port_data);
            impl->portNames.emplace_back(port_names[i]);
        }

        impl->portCount.store(impl->portData.size());

        // Connect filter (let PipeWire negotiate graph)
        int res = pw_filter_connect(impl->filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0);
        (void)res;

        // The processor was configured by setProcessor() while stopped.
        // Start it before exposing the running backend to realtime processing.
        auto* processor = impl->processor.load(std::memory_order_acquire);
        if (processor)
            processor->start();

        impl->filterCreated.store(true);
        impl->cv.notify_all();

        impl->running.store(true);

        // Control thread loops; realtime diagnostics are not printed here
        // in production builds.

        // Run main loop until stop signaled
        while (true) {
            if (impl->threadShouldExit) break;
            pw_loop_iterate(impl->loop, 50);
        }

        // Disconnect and destroy filter
        if (impl->filter) {
            pw_filter_disconnect(impl->filter);
            pw_filter_destroy(impl->filter);
            impl->filter = nullptr;
        }

        auto* stoppingProcessor = impl->processor.load(std::memory_order_acquire);
        if (stoppingProcessor)
            stoppingProcessor->stop();

        impl->running.store(false);
    });

    // Wait briefly for start outcome (filter created or startFailed)
    {
        std::unique_lock<std::mutex> lk(impl_->mtx);
        if (!impl_->cv.wait_for(lk, std::chrono::milliseconds(500), [this]{
                return impl_->filterCreated.load() || impl_->startFailed.load();
            })) {
            // timeout, but we don't fail startup here; let control thread continue
        }
    }

    if (impl_->startFailed.load()) {
        if (impl_->controlThread.joinable())
            impl_->controlThread.join();
        return false;
    }

    return true;
}

bool Backend::stop()
{
    // Signal thread to exit and join it if it's joinable. We must join even
    // if `running` is false because the control thread may have exited
    // early on startup (e.g. failure to connect) and still be joinable.
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->threadShouldExit = true;
    }
    impl_->cv.notify_all();

    if (impl_->controlThread.joinable())
        impl_->controlThread.join();

    impl_->filterCreated.store(false);
    impl_->nodeId.store(0);
    impl_->portCount.store(0);
    impl_->connectedOrPaused.store(false);

    return true;
}

void Backend::shutdown()
{
    stop();

    if (impl_->main_loop) {
        pw_main_loop_destroy(impl_->main_loop);
        impl_->main_loop = nullptr;
    }

    pw_deinit();

    impl_->initialized.store(false);
}

bool Backend::isRunning() const
{
    return impl_->running.load();
}

uint32_t Backend::nodeId() const
{
    if (!impl_->filter) return 0;
    return pw_filter_get_node_id(impl_->filter);
}

std::size_t Backend::portCount() const
{
    return impl_->portCount.load();
}

bool Backend::filterCreated() const
{
    return impl_->filterCreated.load();
}

bool Backend::isConnectedOrPaused() const
{
    return impl_->connectedOrPaused.load();
}

void Backend::setRealtimeCallback(RtCallback cb, void* userData)
{
    impl_->rtCb = cb;
    impl_->rtUser = userData;
}

void Backend::setProcessor(core::IProcessor* processor, void* userData)
{
    (void)userData;

    if (processor) {
        // Configure completely before publishing the processor pointer.
        processor->configure(
            impl_->cfg.sampleRate,
            2,
            impl_->cfg.framesPerBlock
        );
    }

    // Publish only after configuration is complete.
    impl_->processor.store(
        processor,
        std::memory_order_release
    );

    // If the backend is already running, start the processor immediately.
    if (processor && impl_->running.load(std::memory_order_acquire))
        processor->start();
}

} // namespace audiofx::pipewire
