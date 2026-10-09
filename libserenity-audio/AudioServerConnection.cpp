/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AudioServerConnection.h"
#include "Mixer.h"

namespace LibSerenityAudio {

AudioServerConnection::~AudioServerConnection()
{
    teardown();
}

NonnullRefPtr<ClientAudioStream> AudioServerConnection::ensure_stream()
{
    if (!m_stream) {
        auto stream = adopt_ref(*new ClientAudioStream);
        stream->set_sample_rate(m_sample_rate);
        stream->set_volume(m_volume);
        stream->set_muted(m_muted);
        the_mixer().add_stream(stream);
        m_stream = stream;
    }
    return *m_stream;
}

void AudioServerConnection::teardown()
{
    if (m_stream) {
        the_mixer().remove_stream(m_stream.ptr());
        m_stream = nullptr;
    }
}

void AudioServerConnection::die()
{
    teardown();
}

void AudioServerConnection::set_self_sample_rate(u32 sample_rate)
{
    m_sample_rate = sample_rate;
    if (m_stream)
        m_stream->set_sample_rate(sample_rate);
    // Start (or keep) the output at the client's rate; PipeWire resamples to the
    // device as needed.
    PipeWireBackend::the().start(sample_rate);
}

void AudioServerConnection::set_buffer(Audio::AudioQueue const& buffer)
{
    if (!buffer.is_valid())
        return;
    // The IPC decoder hands us an owned queue wrapped in a const ref; the real
    // AudioServer does the same const_cast, and nobody else uses it afterwards.
    ensure_stream()->set_buffer(make<Audio::AudioQueue>(move(const_cast<Audio::AudioQueue&>(buffer))));
}

}
