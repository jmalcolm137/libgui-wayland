/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AudioManagerConnection.h"
#include "Mixer.h"
#include "PipeWireBackend.h"

namespace LibSerenityAudio {

Vector<NonnullRefPtr<AudioManagerConnection>>& AudioManagerConnection::connections()
{
    static auto* connections = new Vector<NonnullRefPtr<AudioManagerConnection>>;
    return *connections;
}

AudioManagerConnection::~AudioManagerConnection()
{
    connections().remove_first_matching([&](auto const& connection) { return connection.ptr() == this; });
}

void AudioManagerConnection::die()
{
    connections().remove_first_matching([&](auto const& connection) { return connection.ptr() == this; });
}

void AudioManagerConnection::for_each(Function<void(AudioManagerConnection&)> callback)
{
    // Copy first: a callback may tear a connection down (e.g. a dying client).
    Vector<NonnullRefPtr<AudioManagerConnection>> snapshot = connections();
    for (auto& connection : snapshot)
        callback(*connection);
}

void AudioManagerConnection::set_main_mix_muted(bool muted)
{
    the_mixer().set_muted(muted);
    for_each([&](auto& connection) { connection.async_main_mix_muted_state_changed(muted); });
}

Messages::AudioManagerServer::IsMainMixMutedResponse AudioManagerConnection::is_main_mix_muted()
{
    return Messages::AudioManagerServer::IsMainMixMutedResponse { the_mixer().is_muted() };
}

Messages::AudioManagerServer::GetMainMixVolumeResponse AudioManagerConnection::get_main_mix_volume()
{
    return Messages::AudioManagerServer::GetMainMixVolumeResponse { the_mixer().main_volume() };
}

void AudioManagerConnection::set_main_mix_volume(double volume)
{
    the_mixer().set_main_volume(volume);
    for_each([&](auto& connection) { connection.async_main_mix_volume_changed(volume); });
}

void AudioManagerConnection::set_device_sample_rate(u32 sample_rate)
{
    // The output is started at the first client's rate; changing it restarts the
    // PipeWire stream at the new rate.
    PipeWireBackend::the().start(sample_rate);
    for_each([&](auto& connection) { connection.async_device_sample_rate_changed(sample_rate); });
}

Messages::AudioManagerServer::GetDeviceSampleRateResponse AudioManagerConnection::get_device_sample_rate()
{
    return Messages::AudioManagerServer::GetDeviceSampleRateResponse { PipeWireBackend::the().sample_rate() };
}

}
