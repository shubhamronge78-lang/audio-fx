#include <cassert>
#include <chrono>
#include <thread>
#include <iostream>
#include <atomic>
#include <cstdlib>
#include <map>
#include <string>
#include <mutex>
#include <condition_variable>

#include "../linux/pipewire_backend.h"
#include "../core/processor.h"

#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <spa/param/audio/format.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/pod/builder.h>
#include <spa/param/format-utils.h>
#include <spa/buffer/buffer.h>
#include <strings.h>


// This test creates two synthetic pw_stream producers (mono FL and FR)
// that should be autoconnected to the AudioFX filter inputs.

using audiofx::pipewire::Backend;
using audiofx::pipewire::Config;
using audiofx::core::IProcessor;

// Synthetic producer descriptor used by callbacks
struct Synthetic {
    pw_stream* stream = nullptr;
    std::atomic<bool> streaming{false};
    float fill_value = 0.0f;
    std::atomic<uint64_t> process_count{0};
    std::string name;
    std::atomic<int> state{0};
};

static void on_stream_state_changed(void* data, pw_stream_state old, pw_stream_state state, const char* error)
{
    Synthetic* s = (Synthetic*)data;
    if (state == PW_STREAM_STATE_STREAMING) s->streaming.store(true);
    if (state == PW_STREAM_STATE_UNCONNECTED || state == PW_STREAM_STATE_ERROR) s->streaming.store(false);
    s->state.store((int)state);
    // diagnostic: print state transitions on control thread
    int oldst = (int)old;
    int newst = (int)state;
    std::cerr << "STREAM_STATE_CHANGED: " << (s->name.empty() ? "<unnamed>" : s->name) << " old=" << oldst << " new=" << newst << " error=" << (error?error:"(null)") << std::endl;
}

static void on_stream_process(void* data)
{
    Synthetic* s = (Synthetic*)data;
    pw_stream* stream = s->stream;
    struct pw_buffer* buf = pw_stream_dequeue_buffer(stream);
    // RT-safe signal that process() ran
    s->process_count.fetch_add(1, std::memory_order_relaxed);
    if (!buf) return;
    struct spa_buffer* spa_buf = buf->buffer;
    if (!spa_buf) {
        pw_stream_queue_buffer(stream, buf);
        return;
    }
    for (uint32_t i = 0; i < spa_buf->n_datas; ++i) {
        struct spa_data* d = &spa_buf->datas[i];
        if (d->data && d->chunk->size >= sizeof(float)) {
            uint32_t samples = d->chunk->size / sizeof(float);
            float* ptr = (float*)(d->data);
            for (uint32_t n = 0; n < samples; ++n) ptr[n] = s->fill_value;
        }
    }
    pw_stream_queue_buffer(stream, buf);
}

static void on_sink_process(void* data)
{
    Synthetic* s = (Synthetic*)data;
    pw_stream* stream = s->stream;
    struct pw_buffer* buf = pw_stream_dequeue_buffer(stream);
    // RT-safe signal that sink processed a buffer
    s->process_count.fetch_add(1, std::memory_order_relaxed);
    if (!buf) return;
    // consume but do not modify
    pw_stream_queue_buffer(stream, buf);
}

// Link state tracking for pw_link events (file scope)
struct LinkState {
    std::atomic<int> state{PW_LINK_STATE_INIT};
    std::atomic<bool> error{false};
};

static void link_info_cb(void* data, const struct pw_link_info* info) {
    LinkState* s = (LinkState*)data;
    if (!s) return;
    if (info) {
        s->state.store((int)info->state);
        if (info->state == PW_LINK_STATE_ERROR) s->error.store(true);
    }
}

static struct pw_link_events link_events;

// Invoke helper: wrapper and sync invoke
struct InvokeReq { void (*op)(void*); void* data; };
static int invoke_wrapper(struct spa_loop* /*loop*/, bool /*async*/, uint32_t /*seq*/, const void* data, size_t size, void* /*user_data*/) {
    if (!data || size < sizeof(InvokeReq)) return 0;
    const InvokeReq req = *(const InvokeReq*)data;
    if (req.op) req.op(req.data);
    return 0;
}
static int invoke_sync(pw_loop* loop, void (*op)(void*), void* data) {
    InvokeReq req{op, data};
    return pw_loop_invoke(loop, invoke_wrapper, 0, &req, sizeof(req), true, nullptr);
}

// Request structs and operations for PipeWire control thread
struct StreamCreateReq { pw_core* core; const char* name; float fill; Synthetic* s; pw_stream_events* events; pw_direction dir; };
static void op_stream_create(void* data) {
    StreamCreateReq* r = (StreamCreateReq*)data;
    if (!r) return;
    pw_properties* props = pw_properties_new(
        PW_KEY_NODE_NAME, r->name,
        PW_KEY_APP_NAME, "audiofx-test",
        PW_KEY_MEDIA_CLASS, (r->dir == PW_DIRECTION_OUTPUT) ? "Audio/Source" : "Audio/Sink",
        NULL);
    pw_stream* stream = pw_stream_new(r->core, r->name, props);
    r->s->stream = stream;
    r->s->fill_value = r->fill;
    r->s->name = r->name;
    struct spa_hook* hook = (struct spa_hook*)calloc(1, sizeof(struct spa_hook));
    pw_stream_add_listener(stream, hook, r->events, r->s);
    // Build an explicit SPA audio format pod (F32, 48000, 1) and pass as params
    uint8_t pod_buf[256];
    struct spa_pod_builder b;
    spa_pod_builder_init(&b, pod_buf, sizeof(pod_buf));
    struct spa_audio_info_raw info;
    memset(&info, 0, sizeof(info));
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = 48000;
    info.channels = 1;
    const struct spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    int res = pw_stream_connect(stream,
        r->dir,
        PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
        params,
        params[0] ? 1 : 0);
    // diagnostic: print connect result and initial stream state
    std::cerr << "pw_stream_connect " << r->name << " result=" << res << std::endl;
    const char* state_error = nullptr;
    int st = static_cast<int>(pw_stream_get_state(stream, &state_error));
    std::cerr << "pw_stream_get_state " << r->name << " => " << st << " error=" << (state_error?state_error:"(null)") << std::endl;
}

struct LinkCreateReq { pw_core* core; uint32_t out_node; uint32_t out_port; uint32_t in_node; uint32_t in_port; void** out_proxy; };
static void op_link_create(void* data) {
    LinkCreateReq* r = (LinkCreateReq*)data;
    if (!r) return;
    pw_properties* link_props = pw_properties_new(NULL);
    char buf[64];
    snprintf(buf, sizeof(buf), "%u", r->out_node);
    pw_properties_set(link_props, PW_KEY_LINK_OUTPUT_NODE, buf);
    snprintf(buf, sizeof(buf), "%u", r->out_port);
    pw_properties_set(link_props, PW_KEY_LINK_OUTPUT_PORT, buf);
    snprintf(buf, sizeof(buf), "%u", r->in_node);
    pw_properties_set(link_props, PW_KEY_LINK_INPUT_NODE, buf);
    snprintf(buf, sizeof(buf), "%u", r->in_port);
    pw_properties_set(link_props, PW_KEY_LINK_INPUT_PORT, buf);
    void* proxy = pw_core_create_object(r->core,
        "link-factory",
        PW_TYPE_INTERFACE_Link,
        PW_VERSION_LINK,
        &link_props->dict,
        0);
    pw_properties_free(link_props);
    if (r->out_proxy) *r->out_proxy = proxy;
}

struct BindReq { pw_registry* registry; uint32_t global; void** out_proxy; LinkState* state; struct spa_hook* hook; };
static void op_bind(void* data) {
    BindReq* r = (BindReq*)data;
    if (!r) return;
    void* proxy = pw_registry_bind(r->registry, r->global, PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, 0);
    if (r->out_proxy) *r->out_proxy = proxy;
    if (proxy && r->hook) {
        pw_link_add_listener((pw_link*)proxy, r->hook, &link_events, r->state);
    }
}

struct DestroyProxyReq { void* proxy; };
static void op_destroy_proxy(void* data) {
    DestroyProxyReq* r = (DestroyProxyReq*)data;
    if (!r) return;
    if (r->proxy) pw_proxy_destroy((pw_proxy*)r->proxy);
}

struct ProxyAddListenerReq { void* proxy; LinkState* state; struct spa_hook* hook; };
static void op_add_link_listener(void* data) {
    ProxyAddListenerReq* r = (ProxyAddListenerReq*)data;
    if (!r) return;
    if (r->proxy && r->hook && r->state) {
        pw_link_add_listener((pw_link*)r->proxy, r->hook, &link_events, r->state);
    }
}

struct DestroyStreamReq { pw_stream* s; };
static void op_destroy_stream(void* data) {
    DestroyStreamReq* r = (DestroyStreamReq*)data;
    if (!r) return;
    if (r->s) {
        pw_stream_disconnect(r->s);
        pw_stream_destroy(r->s);
    }
}

struct QuitReq { pw_main_loop* loop; };
static void op_quit(void* data) {
    QuitReq* q = (QuitReq*)data;
    if (q && q->loop) pw_main_loop_quit(q->loop);
}

int main()
{
    Backend b;
    Config cfg;
    cfg.sampleRate = 48000;
    cfg.framesPerBlock = 128;

    assert(b.initialize(cfg));

    std::atomic<uint64_t> counter{0};

    struct TestProc : IProcessor {
        std::atomic<uint64_t>* cnt;
        void configure(uint32_t, uint32_t, uint32_t) noexcept override {}
        void start() noexcept override {}
        void stop() noexcept override {}
        void process(const audiofx::core::AudioBlock& block) noexcept override {
            // increment once per callback in RT-safe manner
            cnt->fetch_add(1, std::memory_order_relaxed);
            // optional pass-through
            for (uint32_t c = 0; c < block.channels; ++c) {
                float* in = block.inputs[c];
                float* out = block.outputs[c];
                for (uint32_t f = 0; f < block.frames; ++f) out[f] = in[f];
            }
        }
    } proc;
    proc.cnt = &counter;

    // Install processor before starting the backend so that
    // Backend::start() can call processor->start() as part of
    // the controlled lifecycle: configure -> start -> process -> stop.
    b.setProcessor(&proc, nullptr);

    assert(b.start());

    // Initialize PipeWire for the test synthetic sources
    pw_init(nullptr, nullptr);
    pw_main_loop* mloop = pw_main_loop_new(nullptr);
    pw_context* context = pw_context_new(pw_main_loop_get_loop(mloop), nullptr, 0);
    pw_core* core = pw_context_connect(context, nullptr, 0);

    // --- DIAGNOSTIC (Step 7): core error listener ---
    struct spa_hook core_diag_hook;
    static const struct pw_core_events core_diag_events = {
        .version = PW_VERSION_CORE_EVENTS,
        .error = [](void* /*data*/, uint32_t id, int seq, int res, const char* message) {
            std::cerr << "CORE_ERROR:\n"
                      << "  id=" << id << "\n"
                      << "  seq=" << seq << "\n"
                      << "  res=" << res << "\n"
                      << "  message=" << (message ? message : "(null)") << "\n";
        },
    };
    pw_core_add_listener(core, &core_diag_hook, &core_diag_events, nullptr);

    // Registry and discovery state
    struct Discovery {
        uint32_t audiofx_node_id = 0;
        uint32_t audiofx_input_fl_port = 0;
        uint32_t audiofx_input_fr_port = 0;
        uint32_t audiofx_output_fl_port = 0;
        uint32_t audiofx_output_fr_port = 0;
        uint32_t synth_fl_node = 0;
        uint32_t synth_fr_node = 0;
        uint32_t synth_fl_port = 0;
        uint32_t synth_fr_port = 0;
        uint32_t sink_fl_node = 0;
        uint32_t sink_fr_node = 0;
        uint32_t sink_fl_port = 0;
        uint32_t sink_fr_port = 0;
        uint32_t link_fl_global = 0;
        uint32_t link_fr_global = 0;
        uint32_t link_out_fl_global = 0;
        uint32_t link_out_fr_global = 0;
    } disc;

    std::mutex disc_mtx;
    std::condition_variable disc_cv;

    // Registry listener: discover nodes and ports by props
    pw_registry* registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    struct spa_hook reg_hook;
    // pending ports by node id for ports that arrive before node global
    std::map<uint32_t, std::vector<std::pair<uint32_t, std::string>>> pending_ports;

    // registry context struct to pass discovery, sync objects and pending_ports
    struct RegistryCtx { Discovery* d; std::mutex* m; std::condition_variable* cv; decltype(pending_ports)* pending; };
    static const pw_registry_events reg_events = {
        PW_VERSION_REGISTRY_EVENTS,
        // global
        [](void* data, uint32_t id, uint32_t permissions, const char* type, uint32_t version, const struct spa_dict* props) {
            auto* ctx = (RegistryCtx*)data;
            Discovery* d = ctx->d;
            std::mutex* m = ctx->m;
            std::condition_variable* cv = ctx->cv;
            const char* node_name = props ? spa_dict_lookup(props, PW_KEY_NODE_NAME) : nullptr;
            const char* port_name = props ? spa_dict_lookup(props, PW_KEY_PORT_NAME) : nullptr;
            const char* node_id_str = props ? spa_dict_lookup(props, PW_KEY_NODE_ID) : nullptr;
            // AudioFX node
            if (type && strcmp(type, PW_TYPE_INTERFACE_Node) == 0 && node_name && strcmp(node_name, "AudioFX") == 0) {
                std::lock_guard<std::mutex> lk(*m);
                d->audiofx_node_id = id;
                cv->notify_all();
                return;
            }
            // Port globals: match by name, direction and node association
            if (type && strcmp(type, PW_TYPE_INTERFACE_Port) == 0 && port_name) {
                std::lock_guard<std::mutex> lk(*m);
                // attempt to read direction from well-known dict keys using spa_dict_lookup
                const char* dir_val = nullptr;
                if (props) {
                    dir_val = spa_dict_lookup(props, "port.direction");
                    if (!dir_val) dir_val = spa_dict_lookup(props, "direction");
                    if (!dir_val) dir_val = spa_dict_lookup(props, "PortDirection");
                    // as a last resort, try a few common variants
                    if (!dir_val) dir_val = spa_dict_lookup(props, "port.direction.raw");
                }
                if (node_id_str) {
                    uint32_t nid = (uint32_t)atoi(node_id_str);
                    // --- DIAGNOSTIC (Step 5): log every port unconditionally ---
                    std::cerr << "ANY_PORT: id=" << id
                              << " name=" << port_name
                              << " dir=" << (dir_val ? dir_val : "?")
                              << " node.id=" << nid << std::endl;
                    // Check if this port belongs to AudioFX
                    if (nid == d->audiofx_node_id) {
                        // require both name and direction to match intended mapping
                        if (dir_val && strcmp(dir_val, "in") == 0) {
                            if (strcmp(port_name, "input_FL") == 0) d->audiofx_input_fl_port = id;
                            if (strcmp(port_name, "input_FR") == 0) d->audiofx_input_fr_port = id;
                        } else if (dir_val && strcmp(dir_val, "out") == 0) {
                            if (strcmp(port_name, "output_FL") == 0) d->audiofx_output_fl_port = id;
                            if (strcmp(port_name, "output_FR") == 0) d->audiofx_output_fr_port = id;
                        }
                        // diagnostic
                        std::cerr << "DISCOVERED PORT: id=" << id << " name=" << port_name << " dir=" << (dir_val?dir_val:"?") << " node=" << nid << std::endl;
                        cv->notify_all();
                        return;
                    }
                    // If belongs to a synthetic node already seen, assign (sources are outputs)
                    if (nid == d->synth_fl_node) { d->synth_fl_port = id; cv->notify_all(); return; }
                    if (nid == d->synth_fr_node) { d->synth_fr_port = id; cv->notify_all(); return; }
                    // If belongs to sink nodes (sinks are inputs) assign
                    // FIX: sink nodes expose both a playback (dir=in) port and a
                    // monitor (dir=out) port; only the playback/input port is a
                    // valid link.input.port target. Without this check, whichever
                    // port arrived last (often the monitor/output port) silently
                    // overwrote sink_fl_port/sink_fr_port, causing link-factory to
                    // reject the output link with EINVAL (two output ports).
                    if (nid == d->sink_fl_node) {
                        if (dir_val && strcmp(dir_val, "in") == 0) { d->sink_fl_port = id; cv->notify_all(); }
                        return;
                    }
                    if (nid == d->sink_fr_node) {
                        if (dir_val && strcmp(dir_val, "in") == 0) { d->sink_fr_port = id; cv->notify_all(); }
                        return;
                    }
                    // Otherwise store pending
                    auto* pending = ctx->pending;
                    if (pending) pending->operator[](nid).push_back(std::make_pair(id, std::string(port_name)));
                }
            }
            // Link globals: capture when link props match our expected endpoints
            if (type && strcmp(type, PW_TYPE_INTERFACE_Link) == 0 && props) {
                // --- DIAGNOSTIC (Step 9): log every Link global unconditionally ---
                {
                    const char* lon = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE);
                    const char* lop = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_PORT);
                    const char* lin = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);
                    const char* lip = spa_dict_lookup(props, PW_KEY_LINK_INPUT_PORT);
                    std::cerr << "LINK_GLOBAL:\n"
                              << "  id=" << id << "\n"
                              << "  permissions=" << permissions << "\n"
                              << "  link.output.node=" << (lon ? lon : "?") << "\n"
                              << "  link.output.port=" << (lop ? lop : "?") << "\n"
                              << "  link.input.node=" << (lin ? lin : "?") << "\n"
                              << "  link.input.port=" << (lip ? lip : "?") << "\n";
                }
                const char* out_node = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE);
                const char* in_node = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);
                        if (out_node && in_node) {
                            uint32_t outn = (uint32_t)atoi(out_node);
                            uint32_t inn = (uint32_t)atoi(in_node);
                            std::lock_guard<std::mutex> lk(*m);
                            if (outn == d->synth_fl_node && inn == d->audiofx_node_id) { d->link_fl_global = id; cv->notify_all(); return; }
                            if (outn == d->synth_fr_node && inn == d->audiofx_node_id) { d->link_fr_global = id; cv->notify_all(); return; }
                            if (outn == d->audiofx_node_id && inn == d->sink_fl_node) { d->link_out_fl_global = id; cv->notify_all(); return; }
                            if (outn == d->audiofx_node_id && inn == d->sink_fr_node) { d->link_out_fr_global = id; cv->notify_all(); return; }
                        }
            }

            // Node globals: capture synthetic stream nodes by node name prefix
            if (type && strcmp(type, PW_TYPE_INTERFACE_Node) == 0 && node_name) {
                std::lock_guard<std::mutex> lk(*m);
                if (strcmp(node_name, "audiofx-test-src-FL") == 0) {
                    d->synth_fl_node = id;
                    // assign any pending ports for this node
                    auto* pending = ctx->pending;
                    if (pending) {
                        auto it = pending->find(id);
                        if (it != pending->end()) {
                            for (auto &p : it->second) {
                                if (p.second == "output") d->synth_fl_port = p.first;
                            }
                        }
                    }
                    cv->notify_all(); return;
                }
                if (strcmp(node_name, "audiofx-test-src-FR") == 0) {
                    d->synth_fr_node = id;
                    auto* pending = ctx->pending;
                    if (pending) {
                        auto it = pending->find(id);
                        if (it != pending->end()) {
                            for (auto &p : it->second) {
                                if (p.second == "output") d->synth_fr_port = p.first;
                            }
                        }
                    }
                    cv->notify_all(); return;
                }
                if (strcmp(node_name, "audiofx-test-sink-FL") == 0) {
                    d->sink_fl_node = id;
                    // assign any pending ports for this node (take first)
                    auto* pending = ctx->pending;
                    if (pending) {
                        auto it = pending->find(id);
                        if (it != pending->end() && !it->second.empty()) {
                            d->sink_fl_port = it->second.front().first;
                        }
                    }
                    cv->notify_all(); return;
                }
                if (strcmp(node_name, "audiofx-test-sink-FR") == 0) {
                    d->sink_fr_node = id;
                    auto* pending = ctx->pending;
                    if (pending) {
                        auto it = pending->find(id);
                        if (it != pending->end() && !it->second.empty()) {
                            d->sink_fr_port = it->second.front().first;
                        }
                    }
                    cv->notify_all(); return;
                }
            }
            // Port globals may arrive before node globals; handle in separate pass in main thread
        },
        // global_remove
        [](void* data, uint32_t id) {
            (void)data;
            // --- DIAGNOSTIC (Step 9): log every global removal ---
            std::cerr << "LINK_GLOBAL_REMOVE:\n  id=" << id << "\n";
        }
    };

    auto reg_ctx = new RegistryCtx{&disc, &disc_mtx, &disc_cv, &pending_ports};
    pw_registry_add_listener(registry, &reg_hook, &reg_events, reg_ctx);


    Synthetic srcFL, srcFR;
    static pw_stream_events stream_events;
    stream_events.version = PW_VERSION_STREAM_EVENTS;
    stream_events.state_changed = on_stream_state_changed;
    stream_events.process = on_stream_process;

    static pw_stream_events sink_stream_events;
    sink_stream_events.version = PW_VERSION_STREAM_EVENTS;
    sink_stream_events.state_changed = on_stream_state_changed;
    sink_stream_events.process = on_sink_process;

    // Helper to create a mono stream with given channel name
    auto make_stream = [&](Synthetic& s, const char* name, const char* channel, float fill)->bool {
        // create properties with stable node name and media class
        pw_properties* props = pw_properties_new(
            PW_KEY_NODE_NAME, name,
            PW_KEY_APP_NAME, "audiofx-test",
            PW_KEY_MEDIA_CLASS, "Audio/Source",
            NULL);
        pw_stream* stream = pw_stream_new(core, name, props);
        if (!stream) return false;
        s.stream = stream;
        s.fill_value = fill;
        // attach listener (spa_hook must be stable storage)
        struct spa_hook* hook = (struct spa_hook*)calloc(1, sizeof(struct spa_hook));
        pw_stream_add_listener(stream, hook, &stream_events, &s);

        // Connect stream (no autoconnect). We'll create explicit links later.
        int res = pw_stream_connect(stream,
            PW_DIRECTION_OUTPUT,
            PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
            nullptr,
            0);
        (void)res;
        return true;
    };

    // Start the main loop in a background thread before performing control operations
    std::thread pwloopThread([&]{ pw_main_loop_run(mloop); });

    // PipeWire control operations will be executed via invoke_sync

    // Create FL and FR mono streams with deterministic non-zero samples by invoking on the main loop
    StreamCreateReq reqFL{core, "audiofx-test-src-FL", 0.25f, &srcFL, &stream_events, PW_DIRECTION_OUTPUT};
    StreamCreateReq reqFR{core, "audiofx-test-src-FR", -0.25f, &srcFR, &stream_events, PW_DIRECTION_OUTPUT};
    invoke_sync(pw_main_loop_get_loop(mloop), op_stream_create, &reqFL);
    invoke_sync(pw_main_loop_get_loop(mloop), op_stream_create, &reqFR);

    // Wait up to 3 seconds for both streams to enter STREAMING
    const auto stream_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < stream_deadline) {
        if (srcFL.streaming.load() && srcFR.streaming.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }


    // Wait up to 5 seconds for startup to reach usable state
    const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < ready_deadline) {
        if (b.nodeId() != 0 && b.portCount() == 4 && b.filterCreated() && b.isConnectedOrPaused()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Wait for registry to report AudioFX node and input ports and synthetic nodes/ports
    auto wait_for_disc = [&](auto pred, int timeout_ms)->bool {
        std::unique_lock<std::mutex> lk(disc_mtx);
        return disc_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&]{ return pred(); });
    };

    // Wait for AudioFX node
    bool ok = wait_for_disc([&]{ return disc.audiofx_node_id != 0; }, 3000);
    assert(ok && "AudioFX node not found in registry");

    // Wait for AudioFX input ports
    ok = wait_for_disc([&]{ return disc.audiofx_input_fl_port != 0 && disc.audiofx_input_fr_port != 0; }, 3000);
    assert(ok && "AudioFX input ports not found in registry");

    // Wait for synthetic nodes
    ok = wait_for_disc([&]{ return disc.synth_fl_node != 0 && disc.synth_fr_node != 0; }, 3000);
    assert(ok && "Synthetic stream nodes not discovered");

    // Wait for synthetic ports to be discovered (may come from pending)
    // We'll wait up to 3s
    ok = wait_for_disc([&]{ return disc.synth_fl_port != 0 && disc.synth_fr_port != 0; }, 3000);
    // ports may not be discovered yet; continue even if not found, we'll still create link using node ids

    // Create sink streams (consumers) and attach
    Synthetic sinkFL, sinkFR;
    StreamCreateReq sinkReqFL{core, "audiofx-test-sink-FL", 0.0f, &sinkFL, &sink_stream_events, PW_DIRECTION_INPUT};
    StreamCreateReq sinkReqFR{core, "audiofx-test-sink-FR", 0.0f, &sinkFR, &sink_stream_events, PW_DIRECTION_INPUT};
    invoke_sync(pw_main_loop_get_loop(mloop), op_stream_create, &sinkReqFL);
    invoke_sync(pw_main_loop_get_loop(mloop), op_stream_create, &sinkReqFR);

    // Wait for sink streams to reach PAUSED or higher
    const auto sink_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < sink_deadline) {
        if (sinkFL.state.load() >= PW_STREAM_STATE_PAUSED && sinkFR.state.load() >= PW_STREAM_STATE_PAUSED) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Wait for sink node globals via registry
    ok = wait_for_disc([&]{ return disc.sink_fl_node != 0 && disc.sink_fr_node != 0; }, 3000);
    assert(ok && "Sink nodes not discovered");
    // Wait for sink ports (may come from pending)
    ok = wait_for_disc([&]{ return disc.sink_fl_port != 0 && disc.sink_fr_port != 0; }, 3000);

    // Create explicit links: FL and FR on the PipeWire main-loop thread
    void* fl_link_proxy = nullptr;
    void* fr_link_proxy = nullptr;
    LinkCreateReq creq1{core, disc.synth_fl_node, disc.synth_fl_port ? disc.synth_fl_port : 0, disc.audiofx_node_id, disc.audiofx_input_fl_port, &fl_link_proxy};
    LinkCreateReq creq2{core, disc.synth_fr_node, disc.synth_fr_port ? disc.synth_fr_port : 0, disc.audiofx_node_id, disc.audiofx_input_fr_port, &fr_link_proxy};
    invoke_sync(pw_main_loop_get_loop(mloop), op_link_create, &creq1);
    invoke_sync(pw_main_loop_get_loop(mloop), op_link_create, &creq2);
    assert(fl_link_proxy && "failed to create FL link proxy");
    assert(fr_link_proxy && "failed to create FR link proxy");

    // Wait for registry to report the link globals
    ok = wait_for_disc([&]{ return disc.link_fl_global != 0 && disc.link_fr_global != 0; }, 3000);
    assert(ok && "Link globals not reported by registry");

    // Wait for AudioFX output ports to be discovered
    ok = wait_for_disc([&]{ return disc.audiofx_output_fl_port != 0 && disc.audiofx_output_fr_port != 0; }, 3000);
    assert(ok && "AudioFX output ports not found in registry");

    // Create output links: AudioFX output -> sink inputs
    void* fl_out_link_proxy = nullptr;
    void* fr_out_link_proxy = nullptr;
    LinkCreateReq outreq1{core, disc.audiofx_node_id, disc.audiofx_output_fl_port ? disc.audiofx_output_fl_port : 0, disc.sink_fl_node, disc.sink_fl_port ? disc.sink_fl_port : 0, &fl_out_link_proxy};
    LinkCreateReq outreq2{core, disc.audiofx_node_id, disc.audiofx_output_fr_port ? disc.audiofx_output_fr_port : 0, disc.sink_fr_node, disc.sink_fr_port ? disc.sink_fr_port : 0, &fr_out_link_proxy};
    std::cerr << "Creating output links with props: audiofx_output_fl_port=" << disc.audiofx_output_fl_port << " audiofx_output_fr_port=" << disc.audiofx_output_fr_port << " sink_fl_port=" << disc.sink_fl_port << " sink_fr_port=" << disc.sink_fr_port << "\n";
    // --- DIAGNOSTIC (Step 4): full link property values before creation ---
    std::cerr << "OUTPUT LINK FL: link.output.node=" << disc.audiofx_node_id
              << " link.output.port=" << disc.audiofx_output_fl_port
              << " link.input.node=" << disc.sink_fl_node
              << " link.input.port=" << disc.sink_fl_port << "\n";
    std::cerr << "OUTPUT LINK FR: link.output.node=" << disc.audiofx_node_id
              << " link.output.port=" << disc.audiofx_output_fr_port
              << " link.input.node=" << disc.sink_fr_node
              << " link.input.port=" << disc.sink_fr_port << "\n";
    std::cerr << "Sink states: " << sinkFL.state.load() << ", " << sinkFR.state.load() << "\n";
    invoke_sync(pw_main_loop_get_loop(mloop), op_link_create, &outreq1);
    invoke_sync(pw_main_loop_get_loop(mloop), op_link_create, &outreq2);
    assert(fl_out_link_proxy && "failed to create FL output link proxy");
    assert(fr_out_link_proxy && "failed to create FR output link proxy");

    std::cerr << "Created out link proxies: fl_out_link_proxy=" << fl_out_link_proxy << " fr_out_link_proxy=" << fr_out_link_proxy << "\n";

    // --- DIAGNOSTIC (Step 8): proxy error listeners on output link proxies ---
    static const struct pw_proxy_events link_proxy_diag_events = {
        .version = PW_VERSION_PROXY_EVENTS,
        .destroy = [](void* data) {
            std::cerr << "LINK_PROXY_DESTROYED: " << (data ? (const char*)data : "?") << "\n";
        },
        .bound = [](void* data, uint32_t global_id) {
            std::cerr << "LINK_PROXY_BOUND: " << (data ? (const char*)data : "?")
                      << " global_id=" << global_id << "\n";
        },
        .error = [](void* data, int seq, int res, const char* message) {
            std::cerr << "LINK_PROXY_ERROR: " << (data ? (const char*)data : "?")
                      << "\n  seq=" << seq
                      << "\n  res=" << res
                      << "\n  message=" << (message ? message : "(null)") << "\n";
        },
    };
    struct spa_hook* fl_out_proxy_hook = (struct spa_hook*)calloc(1, sizeof(struct spa_hook));
    struct spa_hook* fr_out_proxy_hook = (struct spa_hook*)calloc(1, sizeof(struct spa_hook));
    if (fl_out_link_proxy) {
        std::cerr << "LINK_PROXY_CREATED: fl_out_link_proxy\n";
        pw_proxy_add_listener((pw_proxy*)fl_out_link_proxy, fl_out_proxy_hook,
                               &link_proxy_diag_events, (void*)"fl_out_link_proxy");
    }
    if (fr_out_link_proxy) {
        std::cerr << "LINK_PROXY_CREATED: fr_out_link_proxy\n";
        pw_proxy_add_listener((pw_proxy*)fr_out_link_proxy, fr_out_proxy_hook,
                               &link_proxy_diag_events, (void*)"fr_out_link_proxy");
    }
    // Wait for registry to report the output link globals generated by link creation
    ok = wait_for_disc([&]{ return disc.link_out_fl_global != 0 && disc.link_out_fr_global != 0; }, 5000);
    if (!ok) std::cerr << "Warning: output link globals not reported within timeout (they may still appear shortly)\n";

    // Bind to link globals and observe state
    LinkState fl_state, fr_state;
    LinkState out_fl_state, out_fr_state;
    struct spa_hook fl_hook, fr_hook;
    link_events.version = PW_VERSION_LINK_EVENTS;
    link_events.info = link_info_cb;

    // Bind to link globals and observe state on the main loop thread
    struct spa_hook* fl_hook_ptr = (struct spa_hook*)calloc(1, sizeof(struct spa_hook));
    struct spa_hook* fr_hook_ptr = (struct spa_hook*)calloc(1, sizeof(struct spa_hook));
    void* fl_link = nullptr;
    void* fr_link = nullptr;
    BindReq breq1{registry, disc.link_fl_global, &fl_link, &fl_state, fl_hook_ptr};
    BindReq breq2{registry, disc.link_fr_global, &fr_link, &fr_state, fr_hook_ptr};
    invoke_sync(pw_main_loop_get_loop(mloop), op_bind, &breq1);
    invoke_sync(pw_main_loop_get_loop(mloop), op_bind, &breq2);
    assert(fl_link && fr_link);

    // Bind output links
    struct spa_hook* out_fl_hook_ptr = (struct spa_hook*)calloc(1, sizeof(struct spa_hook));
    struct spa_hook* out_fr_hook_ptr = (struct spa_hook*)calloc(1, sizeof(struct spa_hook));
    void* fl_out_link = nullptr;
    void* fr_out_link = nullptr;
    BindReq breq3{registry, disc.link_out_fl_global, &fl_out_link, &out_fl_state, out_fl_hook_ptr};
    BindReq breq4{registry, disc.link_out_fr_global, &fr_out_link, &out_fr_state, out_fr_hook_ptr};
    invoke_sync(pw_main_loop_get_loop(mloop), op_bind, &breq3);
    invoke_sync(pw_main_loop_get_loop(mloop), op_bind, &breq4);
    assert(fl_out_link && fr_out_link);

    // Wait for links to reach PAUSED or ACTIVE
    auto wait_link_ready = [&](LinkState& s, int timeout_ms)->bool {
        int waited = 0;
        while (waited < timeout_ms) {
            int st = s.state.load();
            if (st >= PW_LINK_STATE_PAUSED) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            waited += 50;
        }
        return false;
    };

    bool fl_ready = wait_link_ready(fl_state, 3000);
    bool fr_ready = wait_link_ready(fr_state, 3000);
    if (!(fl_ready && fr_ready)) {
        std::cerr << "Link readiness failed: fl_state=" << fl_state.state.load() << " fr_state=" << fr_state.state.load() << "\n";
    }
    assert(fl_ready && fr_ready && "links did not become ready");

    // Wait for output links to be ready
    bool out_fl_ready = wait_link_ready(out_fl_state, 3000);
    bool out_fr_ready = wait_link_ready(out_fr_state, 3000);
    if (!(out_fl_ready && out_fr_ready)) {
        std::cerr << "Output link readiness failed: out_fl_state=" << out_fl_state.state.load() << " out_fr_state=" << out_fr_state.state.load() << "\n";
    }
    assert(out_fl_ready && out_fr_ready && "output links did not become ready");

    // Print discovered IDs and link states for test reporting
    std::cerr << "AudioFX node=" << disc.audiofx_node_id << std::endl;
    std::cerr << "AudioFX input_FL=" << disc.audiofx_input_fl_port << std::endl;
    std::cerr << "AudioFX input_FR=" << disc.audiofx_input_fr_port << std::endl;

    std::cerr << "Synthetic FL node=" << disc.synth_fl_node << std::endl;
    std::cerr << "Synthetic FL port=" << disc.synth_fl_port << std::endl;

    std::cerr << "Synthetic FR node=" << disc.synth_fr_node << std::endl;
    std::cerr << "Synthetic FR port=" << disc.synth_fr_port << std::endl;

    std::cerr << "FL link proxy=" << fl_link_proxy << " global=" << disc.link_fl_global << std::endl;
    std::cerr << "FR link proxy=" << fr_link_proxy << " global=" << disc.link_fr_global << std::endl;

    std::cerr << "FL link state=" << fl_state.state.load() << std::endl;
    std::cerr << "FR link state=" << fr_state.state.load() << std::endl;
    std::cerr << "OUT FL link state=" << out_fl_state.state.load() << std::endl;
    std::cerr << "OUT FR link state=" << out_fr_state.state.load() << std::endl;

    std::cerr << "FL stream state streaming=" << srcFL.streaming.load() << " process_count=" << srcFL.process_count.load() << std::endl;
    std::cerr << "FR stream state streaming=" << srcFR.streaming.load() << " process_count=" << srcFR.process_count.load() << std::endl;

    std::cerr << "Sink FL stream state streaming=" << sinkFL.streaming.load() << " consume_count=" << sinkFL.process_count.load() << std::endl;
    std::cerr << "Sink FR stream state streaming=" << sinkFR.streaming.load() << " consume_count=" << sinkFR.process_count.load() << std::endl;

    std::cerr << "processor counter=" << counter.load(std::memory_order_relaxed) << std::endl;

    // Explicit checks with clear diagnostics
    if (!srcFL.streaming.load()) { std::cerr << "ERROR: FL stream not streaming" << std::endl; assert(false && "FL stream not streaming"); }
    if (!srcFR.streaming.load()) { std::cerr << "ERROR: FR stream not streaming" << std::endl; assert(false && "FR stream not streaming"); }
    if (!fl_link_proxy) { std::cerr << "ERROR: FL link proxy is null" << std::endl; assert(false && "FL link proxy null"); }
    if (!fr_link_proxy) { std::cerr << "ERROR: FR link proxy is null" << std::endl; assert(false && "FR link proxy null"); }
    if (disc.link_fl_global == 0 || disc.link_fr_global == 0) { std::cerr << "ERROR: Link globals missing fl=" << disc.link_fl_global << " fr=" << disc.link_fr_global << std::endl; assert(false && "Link globals missing"); }
    if (disc.audiofx_input_fl_port == 0 || disc.audiofx_input_fr_port == 0) { std::cerr << "ERROR: AudioFX input ports missing" << std::endl; assert(false && "AudioFX input ports missing"); }
    if (fl_state.state.load() < PW_LINK_STATE_PAUSED) { std::cerr << "ERROR: FL link not paused/active state=" << fl_state.state.load() << std::endl; assert(false && "FL link not ready"); }
    if (fr_state.state.load() < PW_LINK_STATE_PAUSED) { std::cerr << "ERROR: FR link not paused/active state=" << fr_state.state.load() << std::endl; assert(false && "FR link not ready"); }
    if (out_fl_state.state.load() < PW_LINK_STATE_PAUSED) { std::cerr << "ERROR: OUT FL link not paused/active state=" << out_fl_state.state.load() << std::endl; assert(false && "OUT FL link not ready"); }
    if (out_fr_state.state.load() < PW_LINK_STATE_PAUSED) { std::cerr << "ERROR: OUT FR link not paused/active state=" << out_fr_state.state.load() << std::endl; assert(false && "OUT FR link not ready"); }
    if (sinkFL.state.load() < PW_STREAM_STATE_PAUSED) { std::cerr << "ERROR: Sink FL not connected state=" << sinkFL.state.load() << std::endl; assert(false && "Sink FL not connected"); }
    if (sinkFR.state.load() < PW_STREAM_STATE_PAUSED) { std::cerr << "ERROR: Sink FR not connected state=" << sinkFR.state.load() << std::endl; assert(false && "Sink FR not connected"); }

    // Wait up to 3 seconds for the processor to be invoked
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (counter.load(std::memory_order_relaxed) > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // The processor should have been called at least once
    assert(counter.load(std::memory_order_relaxed) > 0);

    // Stop synthetic streams and cleanup PipeWire on the main-loop thread
    // destroy link proxies (bound proxies)
    DestroyProxyReq d1{fl_link};
    DestroyProxyReq d2{fr_link};
    DestroyProxyReq d5{fl_out_link};
    DestroyProxyReq d6{fr_out_link};
    invoke_sync(pw_main_loop_get_loop(mloop), op_destroy_proxy, &d1);
    invoke_sync(pw_main_loop_get_loop(mloop), op_destroy_proxy, &d2);
    invoke_sync(pw_main_loop_get_loop(mloop), op_destroy_proxy, &d5);
    invoke_sync(pw_main_loop_get_loop(mloop), op_destroy_proxy, &d6);

    // destroy created link objects
    DestroyProxyReq d3{fl_link_proxy};
    DestroyProxyReq d4{fr_link_proxy};
    DestroyProxyReq d7{fl_out_link_proxy};
    DestroyProxyReq d8{fr_out_link_proxy};
    invoke_sync(pw_main_loop_get_loop(mloop), op_destroy_proxy, &d3);
    invoke_sync(pw_main_loop_get_loop(mloop), op_destroy_proxy, &d4);
    invoke_sync(pw_main_loop_get_loop(mloop), op_destroy_proxy, &d7);
    invoke_sync(pw_main_loop_get_loop(mloop), op_destroy_proxy, &d8);

    // destroy streams on main loop (sources and sinks)
    DestroyStreamReq ds1{srcFL.stream};
    DestroyStreamReq ds2{srcFR.stream};
    DestroyStreamReq ds3{sinkFL.stream};
    DestroyStreamReq ds4{sinkFR.stream};
    invoke_sync(pw_main_loop_get_loop(mloop), op_destroy_stream, &ds1);
    invoke_sync(pw_main_loop_get_loop(mloop), op_destroy_stream, &ds2);
    invoke_sync(pw_main_loop_get_loop(mloop), op_destroy_stream, &ds3);
    invoke_sync(pw_main_loop_get_loop(mloop), op_destroy_stream, &ds4);

    // Quit and join the main loop
    QuitReq qreq{mloop};
    invoke_sync(pw_main_loop_get_loop(mloop), op_quit, &qreq);
    if (pwloopThread.joinable()) pwloopThread.join();

    if (context) pw_context_destroy(context);
    if (mloop) pw_main_loop_destroy(mloop);
    pw_deinit();

    assert(b.stop());
    b.shutdown();

    std::cout << "pipewire_processor_integration_test: PASS\n";
    return 0;
}
