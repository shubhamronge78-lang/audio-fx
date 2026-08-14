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
// diagnostics removed

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

    // Realtime callback: obtain buffers for each port and perform pass-through
    // No allocations or locking here.
    if (!impl->filter)
        return;

    // For each port we dequeue a buffer and get dsp pointer if available.
    // Expect four ports in order: input_FL, input_FR, output_FL, output_FR
    const size_t expected = 4;
    if (impl->portData.size() < expected)
        return;

    // Dequeue buffers
    struct pw_buffer *bufs[4] = {nullptr, nullptr, nullptr, nullptr};
    for (size_t i = 0; i < expected; ++i) {
        void *pd = impl->portData[i];
        bufs[i] = pw_filter_dequeue_buffer(pd);
    }

    // If any input buffer is null, nothing to do
    if (!bufs[0] || !bufs[1] || !bufs[2] || !bufs[3]) {
        // Requeue any buffers we dequeued
        for (size_t i = 0; i < expected; ++i) {
            if (bufs[i])
                pw_filter_queue_buffer(impl->portData[i], bufs[i]);
        }
        return;
    }

    // Obtain SPA buffer pointers
    struct spa_buffer *sbuf_inFL = bufs[0]->buffer;
    struct spa_buffer *sbuf_inFR = bufs[1]->buffer;
    struct spa_buffer *sbuf_outFL = bufs[2]->buffer;
    struct spa_buffer *sbuf_outFR = bufs[3]->buffer;

    if (!sbuf_inFL || !sbuf_inFR || !sbuf_outFL || !sbuf_outFR) {
        for (size_t i = 0; i < expected; ++i) {
            if (bufs[i])
                pw_filter_queue_buffer(impl->portData[i], bufs[i]);
        }
        return;
    }

    // Assume dsp format: float32 planar (one channel per port)
    float *inFL = reinterpret_cast<float*>(sbuf_inFL->datas[0].data);
    float *inFR = reinterpret_cast<float*>(sbuf_inFR->datas[0].data);
    float *outFL = reinterpret_cast<float*>(sbuf_outFL->datas[0].data);
    float *outFR = reinterpret_cast<float*>(sbuf_outFR->datas[0].data);

    // Determine frame count. Use the buffer's chunk size if available.
    uint32_t frames = 0;
    if (sbuf_inFL->datas[0].chunk && sbuf_inFL->datas[0].chunk->size)
        frames = sbuf_inFL->datas[0].chunk->size / sizeof(float);
    else if (sbuf_outFL->datas[0].chunk && sbuf_outFL->datas[0].chunk->size)
        frames = sbuf_outFL->datas[0].chunk->size / sizeof(float);
    else
        frames = 0;

    if (frames == 0) {
        for (size_t i = 0; i < expected; ++i) {
            if (bufs[i])
                pw_filter_queue_buffer(impl->portData[i], bufs[i]);
        }
        return;
    }

    // Validate pointers
    if (!inFL || !inFR || !outFL || !outFR) {
        for (size_t i = 0; i < expected; ++i) {
            if (bufs[i])
                pw_filter_queue_buffer(impl->portData[i], bufs[i]);
        }
        return;
    }

    if (impl->rtCb) {
        impl->rtCb(inFL, inFR, outFL, outFR, frames, impl->rtUser);
    } else {
        // Transparent pass-through
        for (uint32_t i = 0; i < frames; ++i) {
            outFL[i] = inFL[i];
            outFR[i] = inFR[i];
        }
    }

    // Requeue buffers
    for (size_t i = 0; i < expected; ++i) {
        if (bufs[i])
            pw_filter_queue_buffer(impl->portData[i], bufs[i]);
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
            impl->startFailed.store(true);
            impl->cv.notify_all();
            return;
        }

        // Create filter (use pw_loop, not pw_core)
        impl->filter = pw_filter_new_simple(impl->loop, "AudioFX", props, &filter_events, impl);
        if (!impl->filter) {
            impl->startFailed.store(true);
            impl->cv.notify_all();
            return;
        }

        // Add four mono DSP ports with properties
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
        int res = pw_filter_connect(impl->filter, PW_FILTER_FLAG_NONE, nullptr, 0);
        (void)res;

        impl->filterCreated.store(true);
        impl->cv.notify_all();

        impl->running.store(true);

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

} // namespace audiofx::pipewire
