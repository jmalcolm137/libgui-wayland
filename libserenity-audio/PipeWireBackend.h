/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Types.h>
#include <pipewire/stream.h>

struct pw_thread_loop;

namespace LibSerenityAudio {

// Pulls mixed samples from the Mixer and feeds them to a PipeWire playback
// stream. The PipeWire process callback runs on a real-time thread; the shared
// queues are lock-free SPSC, and the Mixer guards its stream list with a mutex.
class PipeWireBackend {
public:
    static PipeWireBackend& the();

    // Idempotent. The first call fixes the output sample rate (and thereby the
    // rate every client stream is resampled to when it differs).
    void start(u32 sample_rate);
    void stop();

    bool is_started() const { return m_stream != nullptr; }
    u32 sample_rate() const { return m_sample_rate; }

private:
    PipeWireBackend() = default;
    static void on_process(void* userdata);
    static void on_state_changed(void* userdata, pw_stream_state old, pw_stream_state state, char const* error);
    static void on_param_changed(void* userdata, uint32_t id, spa_pod const* param);

    pw_thread_loop* m_thread_loop { nullptr };
    pw_stream* m_stream { nullptr };
    u32 m_sample_rate { 48000 };
    bool m_start_attempted { false };
    Atomic<bool> m_logged_first_process { false };
    Atomic<bool> m_logged_samples { false };
    u64 m_callback_count { 0 };
};

}
