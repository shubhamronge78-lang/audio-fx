#include "pipewire_backend.h"

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <pipewire/keys.h>

#include <spa/buffer/buffer.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

    struct pw_main_loop* main_loop = nullptr;
    struct pw_loop* loop = nullptr;
    struct pw_context* context = nullptr;
    struct pw_core* core = nullptr;
    struct pw_filter* filter = nullptr;

    std::thread controlThread;
    std::mutex mtx;
    std::condition_variable cv;
    bool threadShouldExit = false;

    std::vector<void*> portData;
    std::vector<std::string> portNames;

    // Processor is published atomically so the realtime callback can
    // safely observe the configured processor.
    //
    // Processor replacement/removal while realtime processing is active
    // is not supported by the public API.
    std::atomic<core::IProcessor*> processor{nullptr};

    // Fixed planar channel pointer arrays.
    // These are reused for every realtime callback and never allocated
    // from the realtime thread.
    float* inputsPtrs[2] = {nullptr, nullptr};
    float* outputsPtrs[2] = {nullptr, nullptr};
};

Backend::Backend()
    : impl_(new Impl{})
{
}

Backend::~Backend()
{
    shutdown();

    delete impl_;
    impl_ = nullptr;
}

static void filter_state_changed(
    void* data,
    enum pw_filter_state /*old*/,
    enum pw_filter_state state,
    const char* /*error*/)
{
    auto* impl = static_cast<Impl*>(data);

    if (state == PW_FILTER_STATE_PAUSED ||
        state == PW_FILTER_STATE_STREAMING) {
        impl->connectedOrPaused.store(
            true,
            std::memory_order_release
        );
    }
}

static void filter_process(
    void* data,
    struct spa_io_position* position)
{
    auto* impl = static_cast<Impl*>(data);

    if (!impl->filter)
        return;

    if (impl->portData.size() < 4)
        return;

    if (!position)
        return;

    const uint32_t frames = position->clock.duration;

    if (frames == 0)
        return;

    float* inFL = static_cast<float*>(
        pw_filter_get_dsp_buffer(impl->portData[0], frames)
    );

    float* inFR = static_cast<float*>(
        pw_filter_get_dsp_buffer(impl->portData[1], frames)
    );

    float* outFL = static_cast<float*>(
        pw_filter_get_dsp_buffer(impl->portData[2], frames)
    );

    float* outFR = static_cast<float*>(
        pw_filter_get_dsp_buffer(impl->portData[3], frames)
    );

    if (!inFL || !inFR || !outFL || !outFR)
        return;

    // Prepare the fixed planar channel arrays.
    impl->inputsPtrs[0] = inFL;
    impl->inputsPtrs[1] = inFR;

    impl->outputsPtrs[0] = outFL;
    impl->outputsPtrs[1] = outFR;

    // Acquire the processor pointer once for this realtime callback.
    //
    // setProcessor() publishes it with release semantics after
    // configuration has completed.
    auto* processor =
        impl->processor.load(std::memory_order_acquire);

    if (processor) {
        core::AudioBlock block{
            impl->inputsPtrs,
            impl->outputsPtrs,
            frames,
            2,
            impl->cfg.sampleRate
        };

        processor->process(block);
        return;
    }

    if (impl->rtCb) {
        impl->rtCb(
            inFL,
            inFR,
            outFL,
            outFR,
            frames,
            impl->rtUser
        );
        return;
    }

    // Transparent fallback when neither a processor nor a realtime
    // callback is installed.
    for (uint32_t i = 0; i < frames; ++i) {
        outFL[i] = inFL[i];
        outFR[i] = inFR[i];
    }
}

static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    nullptr,             // destroy
    filter_state_changed,
    nullptr,             // io_changed
    nullptr,             // param_changed
    nullptr,             // add_buffer
    nullptr,             // remove_buffer
    filter_process,
    nullptr,             // drained
    nullptr              // command
};

bool Backend::initialize(const Config& cfg)
{
    if (impl_->initialized.load(std::memory_order_acquire))
        return true;

    impl_->cfg = cfg;

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

    impl_->context = pw_context_new(
        impl_->loop,
        nullptr,
        0
    );

    if (!impl_->context) {
        pw_main_loop_destroy(impl_->main_loop);
        impl_->main_loop = nullptr;
        impl_->loop = nullptr;
        return false;
    }

    impl_->initialized.store(
        true,
        std::memory_order_release
    );

    return true;
}

bool Backend::start()
{
    if (!impl_->initialized.load(std::memory_order_acquire))
        return false;

    if (impl_->running.load(std::memory_order_acquire))
        return true;

    if (impl_->controlThread.joinable())
        impl_->controlThread.join();

    impl_->threadShouldExit = false;
    impl_->startFailed.store(
        false,
        std::memory_order_release
    );

    impl_->filterCreated.store(
        false,
        std::memory_order_release
    );

    impl_->connectedOrPaused.store(
        false,
        std::memory_order_release
    );

    impl_->controlThread = std::thread([this]() {
        Impl* impl = this->impl_;

        struct pw_properties* props = pw_properties_new(
            PW_KEY_APP_NAME, "AudioFX",
            PW_KEY_NODE_NAME, "AudioFX",
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Filter",
            PW_KEY_MEDIA_ROLE, "DSP",
            PW_KEY_NODE_DESCRIPTION, "AudioFX DSP Filter",
            nullptr
        );

        if (!props) {
            impl->startFailed.store(
                true,
                std::memory_order_release
            );
            impl->cv.notify_all();
            return;
        }

        impl->core = pw_context_connect(
            impl->context,
            nullptr,
            0
        );

        if (!impl->core) {
            const int err = errno;

            std::fprintf(
                stderr,
                "pw_context_connect() failed: %s (%d)\n",
                std::strerror(err),
                err
            );

            impl->startFailed.store(
                true,
                std::memory_order_release
            );

            impl->cv.notify_all();
            return;
        }

        impl->filter = pw_filter_new_simple(
            impl->loop,
            "AudioFX",
            props,
            &filter_events,
            impl
        );

        if (!impl->filter) {
            const int err = errno;

            std::fprintf(
                stderr,
                "pw_filter_new_simple() failed: %s (%d)\n",
                std::strerror(err),
                err
            );

            impl->startFailed.store(
                true,
                std::memory_order_release
            );

            impl->cv.notify_all();
            return;
        }

        const char* portNames[4] = {
            "input_FL",
            "input_FR",
            "output_FL",
            "output_FR"
        };

        const enum pw_direction directions[4] = {
            PW_DIRECTION_INPUT,
            PW_DIRECTION_INPUT,
            PW_DIRECTION_OUTPUT,
            PW_DIRECTION_OUTPUT
        };

        const char* channels[4] = {
            "FL",
            "FR",
            "FL",
            "FR"
        };

        impl->portData.clear();
        impl->portNames.clear();

        for (int i = 0; i < 4; ++i) {
            struct pw_properties* portProperties =
                pw_properties_new(
                    PW_KEY_FORMAT_DSP,
                    "32 bit float mono audio",
                    PW_KEY_AUDIO_CHANNEL,
                    channels[i],
                    nullptr
                );

            if (!portProperties) {
                impl->startFailed.store(
                    true,
                    std::memory_order_release
                );

                impl->cv.notify_all();
                return;
            }

            void* portData = pw_filter_add_port(
                impl->filter,
                directions[i],
                PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                sizeof(void*),
                portProperties,
                nullptr,
                0
            );

            if (!portData) {
                impl->startFailed.store(
                    true,
                    std::memory_order_release
                );

                impl->cv.notify_all();
                return;
            }

            impl->portData.push_back(portData);
            impl->portNames.emplace_back(portNames[i]);
        }

        impl->portCount.store(
            impl->portData.size(),
            std::memory_order_release
        );

        const int result = pw_filter_connect(
            impl->filter,
            PW_FILTER_FLAG_RT_PROCESS,
            nullptr,
            0
        );

        if (result < 0) {
            std::fprintf(
                stderr,
                "pw_filter_connect() failed: %s (%d)\n",
                std::strerror(-result),
                result
            );

            impl->startFailed.store(
                true,
                std::memory_order_release
            );

            impl->cv.notify_all();
            return;
        }

        // The processor was fully configured by setProcessor()
        // while the backend was stopped.
        //
        // Start it before exposing the running backend to realtime
        // processing. This transition is guarded by impl->mtx so it
        // cannot interleave with a concurrent setProcessor() call,
        // which takes the same lock to check/publish the processor.
        {
            std::lock_guard<std::mutex> lock(impl->mtx);

            auto* processor =
                impl->processor.load(std::memory_order_acquire);

            if (processor)
                processor->start();

            impl->filterCreated.store(
                true,
                std::memory_order_release
            );

            impl->running.store(
                true,
                std::memory_order_release
            );
        }

        impl->cv.notify_all();

        while (true) {
            bool shouldExit = false;

            {
                std::lock_guard<std::mutex> lock(impl->mtx);
                shouldExit = impl->threadShouldExit;
            }

            if (shouldExit)
                break;

            pw_loop_iterate(impl->loop, 50);
        }

        if (impl->filter) {
            pw_filter_disconnect(impl->filter);
            pw_filter_destroy(impl->filter);
            impl->filter = nullptr;
        }

        // Stop the same processor that was active during this run.
        // Guarded by impl->mtx for the same reason as the start-side
        // transition above.
        {
            std::lock_guard<std::mutex> lock(impl->mtx);

            auto* stoppingProcessor =
                impl->processor.load(std::memory_order_acquire);

            if (stoppingProcessor)
                stoppingProcessor->stop();

            impl->running.store(
                false,
                std::memory_order_release
            );
        }
    });

    {
        std::unique_lock<std::mutex> lock(impl_->mtx);

        impl_->cv.wait_for(
            lock,
            std::chrono::milliseconds(500),
            [this] {
                return impl_->filterCreated.load(
                           std::memory_order_acquire
                       ) ||
                       impl_->startFailed.load(
                           std::memory_order_acquire
                       );
            }
        );
    }

    if (impl_->startFailed.load(std::memory_order_acquire)) {
        if (impl_->controlThread.joinable())
            impl_->controlThread.join();

        impl_->filterCreated.store(
            false,
            std::memory_order_release
        );

        impl_->running.store(
            false,
            std::memory_order_release
        );

        return false;
    }

    return true;
}

bool Backend::stop()
{
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->threadShouldExit = true;
    }

    impl_->cv.notify_all();

    if (impl_->controlThread.joinable())
        impl_->controlThread.join();

    impl_->filterCreated.store(
        false,
        std::memory_order_release
    );

    impl_->nodeId.store(
        0,
        std::memory_order_release
    );

    impl_->portCount.store(
        0,
        std::memory_order_release
    );

    impl_->connectedOrPaused.store(
        false,
        std::memory_order_release
    );

    impl_->running.store(
        false,
        std::memory_order_release
    );

    return true;
}

void Backend::shutdown()
{
    stop();

    if (impl_->main_loop) {
        pw_main_loop_destroy(impl_->main_loop);
        impl_->main_loop = nullptr;
        impl_->loop = nullptr;
    }

    impl_->context = nullptr;
    impl_->core = nullptr;

    pw_deinit();

    impl_->initialized.store(
        false,
        std::memory_order_release
    );
}

bool Backend::isRunning() const
{
    return impl_->running.load(
        std::memory_order_acquire
    );
}

uint32_t Backend::nodeId() const
{
    if (!impl_->filter)
        return 0;

    return pw_filter_get_node_id(
        impl_->filter
    );
}

std::size_t Backend::portCount() const
{
    return impl_->portCount.load(
        std::memory_order_acquire
    );
}

bool Backend::filterCreated() const
{
    return impl_->filterCreated.load(
        std::memory_order_acquire
    );
}

bool Backend::isConnectedOrPaused() const
{
    return impl_->connectedOrPaused.load(
        std::memory_order_acquire
    );
}

void Backend::setRealtimeCallback(
    RtCallback cb,
    void* userData)
{
    impl_->rtCb = cb;
    impl_->rtUser = userData;
}

void Backend::setProcessor(core::IProcessor* processor, void* userData)
{
    (void)userData;

    // Processor replacement/removal while the backend is running is
    // not supported (see header comment). The check is taken under
    // impl_->mtx -- the same mutex Backend::start()/stop() hold while
    // transitioning impl_->running -- so this cannot race with those
    // transitions. This lock is never taken in filter_process(), so
    // the realtime callback is unaffected.
    std::lock_guard<std::mutex> lock(impl_->mtx);

    if (impl_->running.load(std::memory_order_acquire)) {
        return;
    }

    // Configure the processor completely before publishing it
    // to the realtime thread.
    if (processor) {
        processor->configure(
            impl_->cfg.sampleRate,
            2,
            impl_->cfg.framesPerBlock
        );
    }

    // Publish only after configure() has completed.
    impl_->processor.store(
        processor,
        std::memory_order_release
    );
}


} // namespace audiofx::pipewire
