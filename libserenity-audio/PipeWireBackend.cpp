/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PipeWireBackend.h"
#include "Mixer.h"
#include <AK/Math.h>
#include <LibCore/System.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <stdlib.h>

namespace LibSerenityAudio {

PipeWireBackend& PipeWireBackend::the()
{
    static auto* backend = new PipeWireBackend;
    return *backend;
}

void PipeWireBackend::on_state_changed(void* userdata, pw_stream_state old, pw_stream_state state, char const* error)
{
    (void)userdata;
    dbgln("LibSerenityAudio: PipeWire stream state {} -> {} ({})", pw_stream_state_as_string(old), pw_stream_state_as_string(state), error ? error : "");
}

void PipeWireBackend::on_param_changed(void* userdata, uint32_t id, spa_pod const* param)
{
    (void)userdata;
    if (id != SPA_PARAM_Format || !param)
        return;
    spa_audio_info_raw info {};
    if (spa_format_audio_raw_parse(param, &info) < 0)
        return;
    dbgln("LibSerenityAudio: PipeWire negotiated format={} rate={} channels={}", static_cast<int>(info.format), info.rate, info.channels);
}

void PipeWireBackend::on_process(void* userdata)
{
    auto& self = *static_cast<PipeWireBackend*>(userdata);
    if (!self.m_logged_first_process) {
        self.m_logged_first_process = true;
        dbgln("LibSerenityAudio: PipeWire process callback running");
    }
    auto* buffer = pw_stream_dequeue_buffer(self.m_stream);
    if (!buffer)
        return;

    auto* data = &buffer->buffer->datas[0];
    auto* destination = static_cast<float*>(data->data);
    u32 const stride = sizeof(float) * 2;
    if (!destination || !data->chunk) {
        pw_stream_queue_buffer(self.m_stream, buffer);
        return;
    }

    u32 max_frames = data->maxsize / stride;
    // PipeWire tells us how many frames it wants this cycle; producing the whole
    // (much larger) buffer would add latency and over/underrun the client queue.
    u32 frames = max_frames;
    if (buffer->requested > 0)
        frames = min<u64>(buffer->requested, max_frames);

    auto& mixer = the_mixer();
    float peak = 0;
    for (u32 i = 0; i < frames; ++i) {
        auto sample = mixer.next_sample(self.m_sample_rate);
        destination[i * 2 + 0] = sample.left;
        destination[i * 2 + 1] = sample.right;
        peak = max(peak, max(AK::fabs(sample.left), AK::fabs(sample.right)));
    }

    if (!self.m_logged_samples) {
        self.m_logged_samples = true;
        dbgln("LibSerenityAudio: first callback: requested={} max_frames={} using {} frames", buffer->requested, max_frames, frames);
    }

    if (++self.m_callback_count % 100 == 0 && getenv("LIBWM_AUDIO_TRACE"))
        dbgln("LibSerenityAudio: peak={} streams={} queued_buffers={} underruns={}", peak, mixer.stream_count(), mixer.max_buffered(), mixer.underrun_count());

    data->chunk->offset = 0;
    data->chunk->stride = static_cast<int32_t>(stride);
    data->chunk->size = frames * stride;
    buffer->size = frames;

    pw_stream_queue_buffer(self.m_stream, buffer);
}

void PipeWireBackend::start(u32 sample_rate)
{
    if (m_start_attempted)
        return;
    m_start_attempted = true;
    m_sample_rate = sample_rate;

    m_thread_loop = pw_thread_loop_new("libserenity-audio", nullptr);
    if (!m_thread_loop) {
        dbgln("LibSerenityAudio: failed to create the PipeWire thread loop");
        return;
    }

    pw_thread_loop_lock(m_thread_loop);

    static pw_stream_events const stream_events = {
        .version = PW_VERSION_STREAM_EVENTS,
        .state_changed = on_state_changed,
        .param_changed = on_param_changed,
        .process = on_process,
    };

    // Ask for a small quantum so key-to-sound latency stays low; PipeWire will
    // still convert to whatever the graph actually runs at.
    ByteString latency = ByteString::formatted("{}/{}", 256, sample_rate);

    m_stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(m_thread_loop),
        "SerenityOS",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Music",
            PW_KEY_APP_NAME, "LibSerenityAudio",
            PW_KEY_NODE_LATENCY, latency.characters(),
            nullptr),
        &stream_events,
        this);
    if (!m_stream) {
        dbgln("LibSerenityAudio: failed to create the PipeWire stream");
        pw_thread_loop_unlock(m_thread_loop);
        return;
    }

    spa_audio_info_raw info = {};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = sample_rate;
    info.channels = 2;

    uint8_t pod_buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
    spa_pod const* params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);

    pw_stream_connect(
        m_stream,
        PW_DIRECTION_OUTPUT,
        PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
        params,
        1);

    pw_thread_loop_unlock(m_thread_loop);
    pw_thread_loop_start(m_thread_loop);
    dbgln("LibSerenityAudio: PipeWire output stream started at {} Hz", sample_rate);
}

void PipeWireBackend::stop()
{
    if (m_thread_loop) {
        pw_thread_loop_stop(m_thread_loop);
        if (m_stream) {
            pw_stream_destroy(m_stream);
            m_stream = nullptr;
        }
        pw_thread_loop_destroy(m_thread_loop);
        m_thread_loop = nullptr;
    }
    m_start_attempted = false;
}

}
