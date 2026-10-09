/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibCore/Socket.h>
#include <LibIPC/Connection.h>
#include <AudioServer/AudioClientEndpoint.h>
#include <AudioServer/AudioServerEndpoint.h>

#include "AudioServerDefaultStub.h"
#include "ClientAudioStream.h"
#include "PipeWireBackend.h"

namespace LibSerenityAudio {

// The server end of the SerenityOS AudioServer protocol, backed by PipeWire.
//
// One instance lives per connected audio client (Piano and friends). It owns a
// ClientAudioStream registered with the shared Mixer; the PipeWire backend pulls
// mixed samples from that Mixer on its real-time thread.
class AudioServerConnection final
    : public IPC::Connection<AudioServerEndpoint, AudioClientEndpoint>
    , public AudioServerDefaultStub
    , public AudioClientEndpoint::Proxy<AudioServerEndpoint> {
public:
    static NonnullRefPtr<AudioServerConnection> create(NonnullOwnPtr<Core::LocalSocket> socket)
    {
        return adopt_ref(*new AudioServerConnection(move(socket)));
    }

    ~AudioServerConnection();

private:
    explicit AudioServerConnection(NonnullOwnPtr<Core::LocalSocket> socket)
        : IPC::Connection<AudioServerEndpoint, AudioClientEndpoint>(*this, move(socket))
        , AudioClientEndpoint::Proxy<AudioServerEndpoint>(*this, {})
    {
    }

    NonnullRefPtr<ClientAudioStream> ensure_stream();
    void teardown();
    virtual void die() override;

    void set_self_muted(bool muted) override
    {
        m_muted = muted;
        if (m_stream)
            m_stream->set_muted(muted);
    }
    Messages::AudioServer::IsSelfMutedResponse is_self_muted() override
    {
        return Messages::AudioServer::IsSelfMutedResponse { m_muted };
    }
    Messages::AudioServer::GetSelfVolumeResponse get_self_volume() override
    {
        return Messages::AudioServer::GetSelfVolumeResponse { m_volume };
    }
    void set_self_volume(double volume) override
    {
        m_volume = volume;
        if (m_stream)
            m_stream->set_volume(volume);
    }
    void set_self_sample_rate(u32 sample_rate) override;
    Messages::AudioServer::GetSelfSampleRateResponse get_self_sample_rate() override
    {
        return Messages::AudioServer::GetSelfSampleRateResponse { m_sample_rate };
    }
    void set_buffer(Audio::AudioQueue const& buffer) override;
    void clear_buffer() override
    {
        if (m_stream)
            m_stream->clear();
    }
    void start_playback() override
    {
        ensure_stream()->set_paused(false);
        // Fallback start for clients that never call set_self_sample_rate.
        PipeWireBackend::the().start(m_sample_rate);
    }
    void pause_playback() override
    {
        if (m_stream)
            m_stream->set_paused(true);
    }

    RefPtr<ClientAudioStream> m_stream;
    bool m_muted { false };
    double m_volume { 1 };
    // The stream rate the client asked for (44100 for Piano); PipeWire converts
    // to the actual device rate.
    u32 m_sample_rate { 48000 };
};

}
