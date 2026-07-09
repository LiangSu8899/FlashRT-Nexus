/* Example Mindon-side host wrapper for an already adopted Pi0.5 model.
 *
 * The producer still owns checkpoint loading, capture, prompt/state
 * processing, image preprocessing, and action postprocessing. This wrapper
 * only maps application buffers to declared Nexus embedded-session verbs.
 */
#include "nexus/embedded/session.h"

#include <array>
#include <cstdint>

namespace mindon_pi05_example {

enum : uint32_t {
    kPortSwap = 0,
    kPortStaged = 1,
};

struct TickPayload {
    const void* image_payload = nullptr;
    uint64_t image_bytes = 0;

    const void* noise = nullptr;
    uint64_t noise_bytes = 0;

    const char* prompt = nullptr;
    uint64_t prompt_bytes = 0;

    const float* state = nullptr;
    uint64_t state_count = 0;

    void* actions = nullptr;
    uint64_t action_capacity = 0;
    uint64_t action_written = 0;
};

class Pi05EmbeddedHost {
public:
    Pi05EmbeddedHost() = default;
    Pi05EmbeddedHost(const Pi05EmbeddedHost&) = delete;
    Pi05EmbeddedHost& operator=(const Pi05EmbeddedHost&) = delete;

    ~Pi05EmbeddedHost() { close(); }

    int open(cap_model_runtime* model) {
        close();
        if (!model) return CAP_ERR_ARG;

        model_ = model;
        images_port_ = cap_model_find_port(model, "images");
        noise_port_ = cap_model_find_port(model, "noise");
        actions_port_ = cap_model_find_port(model, "actions");
        prompt_port_ = cap_model_find_port(model, "prompt");
        state_port_ = cap_model_find_port(model, "state");
        if (images_port_ < 0 || noise_port_ < 0 || actions_port_ < 0) {
            return CAP_ERR_ARG;
        }
        if (cap_model_port_update(model, images_port_) != kPortStaged ||
            cap_model_port_update(model, noise_port_) != kPortSwap ||
            cap_model_port_update(model, actions_port_) != kPortStaged) {
            return CAP_ERR_ARG;
        }
        if (prompt_port_ >= 0 &&
            cap_model_port_update(model, prompt_port_) != kPortStaged) {
            return CAP_ERR_ARG;
        }
        if (state_port_ >= 0 &&
            cap_model_port_update(model, state_port_) != kPortStaged) {
            return CAP_ERR_ARG;
        }

        nexus_embedded_config cfg{};
        cfg.struct_size = sizeof(cfg);
        cfg.model = model;
        return nexus_embedded_open(&cfg, &session_);
    }

    void close() {
        if (session_) {
            nexus_embedded_close(session_);
            session_ = nullptr;
        }
        model_ = nullptr;
        images_port_ = noise_port_ = actions_port_ = -1;
        prompt_port_ = state_port_ = -1;
    }

    int step(TickPayload* payload, nexus_embedded_tick_result* result) {
        if (!session_ || !model_ || !payload || !payload->actions) {
            return CAP_ERR_ARG;
        }
        if (!payload->image_payload || !payload->image_bytes ||
            !payload->noise || payload->noise_bytes != noise_bytes()) {
            return CAP_ERR_ARG;
        }
        if (payload->prompt && prompt_port_ < 0) return CAP_ERR_ARG;
        if (payload->state && state_port_ < 0) return CAP_ERR_ARG;

        uint64_t n_inputs = 0;
        if (payload->prompt) {
            inputs_[n_inputs++] = make_input(
                "prompt", payload->prompt, payload->prompt_bytes,
                NEXUS_EMBEDDED_SET_INPUT, -1);
        }
        if (payload->state) {
            inputs_[n_inputs++] = make_input(
                "state", payload->state,
                payload->state_count * sizeof(float),
                NEXUS_EMBEDDED_SET_INPUT, -1);
        }
        inputs_[n_inputs++] = make_input(
            "images", payload->image_payload, payload->image_bytes,
            NEXUS_EMBEDDED_SET_INPUT, -1);
        inputs_[n_inputs++] = make_input(
            "noise", payload->noise, payload->noise_bytes,
            NEXUS_EMBEDDED_SWAP, -1);

        nexus_embedded_output output{};
        output.struct_size = sizeof(output);
        output.port = "actions";
        output.data = payload->actions;
        output.capacity = payload->action_capacity;
        output.stream = -1;

        nexus_embedded_tick_result local_result{};
        local_result.struct_size = sizeof(local_result);
        int rc = nexus_embedded_step(
            session_, inputs_.data(), n_inputs, &output, 1,
            result ? result : &local_result);
        payload->action_written = output.written;
        return rc;
    }

    int snapshot(const char* name) {
        return session_ ? nexus_embedded_snapshot(session_, name) : CAP_ERR_ARG;
    }

    int restore(const char* name) {
        return session_ ? nexus_embedded_restore(session_, name) : CAP_ERR_ARG;
    }

    uint64_t fingerprint() const {
        return session_ ? nexus_embedded_fingerprint(session_) : 0;
    }

    const char* last_error() const {
        return session_ ? nexus_embedded_last_error(session_)
                        : "Mindon Pi0.5 host is not open";
    }

private:
    static nexus_embedded_input make_input(const char* port,
                                           const void* data,
                                           uint64_t bytes,
                                           uint32_t update,
                                           int stream) {
        nexus_embedded_input input{};
        input.struct_size = sizeof(input);
        input.port = port;
        input.data = data;
        input.bytes = bytes;
        input.update = update;
        input.stream = stream;
        return input;
    }

    uint64_t noise_bytes() const {
        return noise_port_ >= 0 ? cap_model_port_bytes(
                                      model_, static_cast<uint64_t>(noise_port_))
                                : 0;
    }

    cap_model_runtime* model_ = nullptr;
    nexus_embedded_session* session_ = nullptr;
    int images_port_ = -1;
    int noise_port_ = -1;
    int actions_port_ = -1;
    int prompt_port_ = -1;
    int state_port_ = -1;
    std::array<nexus_embedded_input, 4> inputs_{};
};

}  // namespace mindon_pi05_example
