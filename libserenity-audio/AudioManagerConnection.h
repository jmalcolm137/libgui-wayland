/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibCore/Socket.h>
#include <LibIPC/Connection.h>
#include <AudioServer/AudioManagerClientEndpoint.h>
#include <AudioServer/AudioManagerServerEndpoint.h>

namespace LibSerenityAudio {

// The server end of the SerenityOS AudioServer *manager* protocol (the mixer
// interface the Audio applet uses), backed by the shared Mixer. Each connected
// manager client gets one; volume/mute changes are broadcast to all of them.
class AudioManagerConnection final
    : public IPC::Connection<AudioManagerServerEndpoint, AudioManagerClientEndpoint>
    , public AudioManagerServerStub
    , public AudioManagerClientEndpoint::Proxy<AudioManagerServerEndpoint> {
public:
    static NonnullRefPtr<AudioManagerConnection> create(NonnullOwnPtr<Core::LocalSocket> socket)
    {
        return adopt_ref(*new AudioManagerConnection(move(socket)));
    }

    ~AudioManagerConnection() override;

    static void for_each(Function<void(AudioManagerConnection&)> callback);

private:
    explicit AudioManagerConnection(NonnullOwnPtr<Core::LocalSocket> socket)
        : IPC::Connection<AudioManagerServerEndpoint, AudioManagerClientEndpoint>(*this, move(socket))
        , AudioManagerClientEndpoint::Proxy<AudioManagerServerEndpoint>(*this, {})
    {
        connections().append(*this);
    }

    virtual void die() override;

    void set_main_mix_muted(bool muted) override;
    Messages::AudioManagerServer::IsMainMixMutedResponse is_main_mix_muted() override;
    Messages::AudioManagerServer::GetMainMixVolumeResponse get_main_mix_volume() override;
    void set_main_mix_volume(double volume) override;
    void set_device_sample_rate(u32 sample_rate) override;
    Messages::AudioManagerServer::GetDeviceSampleRateResponse get_device_sample_rate() override;

    static Vector<NonnullRefPtr<AudioManagerConnection>>& connections();
};

}
