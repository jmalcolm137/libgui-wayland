/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Mixer.h"

namespace LibSerenityAudio {

// Fixed attenuation so summing several clients doesn't clip as easily. Mirrors
// Serenity's AudioServer.
static constexpr float SAMPLE_HEADROOM = 0.95f;

void Mixer::add_stream(NonnullRefPtr<ClientAudioStream> stream)
{
    Threading::MutexLocker locker(m_mutex);
    m_streams.append(move(stream));
}

void Mixer::remove_stream(ClientAudioStream* stream)
{
    Threading::MutexLocker locker(m_mutex);
    m_streams.remove_first_matching([&](auto const& entry) { return entry.ptr() == stream; });
}

Audio::Sample Mixer::next_sample(u32 device_sample_rate)
{
    Threading::MutexLocker locker(m_mutex);
    Audio::Sample sum;
    for (auto& stream : m_streams) {
        auto sample = stream->get_next_sample(device_sample_rate);
        if (!sample.is_error())
            sum += sample.value();
    }
    sum *= SAMPLE_HEADROOM;
    sum.clip();
    return sum;
}

size_t Mixer::stream_count()
{
    Threading::MutexLocker locker(m_mutex);
    return m_streams.size();
}

u64 Mixer::underrun_count()
{
    Threading::MutexLocker locker(m_mutex);
    u64 total = 0;
    for (auto& stream : m_streams)
        total += stream->underrun_count();
    return total;
}

size_t Mixer::max_buffered()
{
    Threading::MutexLocker locker(m_mutex);
    size_t most = 0;
    for (auto& stream : m_streams)
        most = max(most, stream->buffered());
    return most;
}

Mixer& the_mixer()
{
    static auto* mixer = new Mixer;
    return *mixer;
}

}
