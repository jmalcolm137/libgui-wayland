/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <LibAudio/Queue.h>
#include <LibAudio/Sample.h>

namespace LibSerenityAudio {

// One connected audio client's stream: the shared ring buffer it fills plus its
// playback state. Modelled on Serenity's AudioServer::ClientAudioStream.
class ClientAudioStream : public RefCounted<ClientAudioStream> {
public:
    enum class ErrorState {
        ClientDisconnected,
        ClientPaused,
        ClientUnderrun,
        ResamplingError,
    };

    ClientAudioStream() = default;
    ~ClientAudioStream() = default;

    // Returns the next sample at `device_sample_rate`, resampling from the
    // client's rate when they differ.
    ErrorOr<Audio::Sample, ErrorState> get_next_sample(u32 device_sample_rate);

    void set_buffer(NonnullOwnPtr<Audio::AudioQueue> buffer) { m_buffer = move(buffer); }
    bool has_buffer() const { return m_buffer != nullptr; }
    void clear();

    void set_paused(bool paused) { m_paused = paused; }
    bool is_paused() const { return m_paused; }
    u32 sample_rate() const { return m_sample_rate; }
    void set_sample_rate(u32 sample_rate) { m_sample_rate = sample_rate; }
    double volume() const { return m_volume; }
    void set_volume(double volume) { m_volume = volume; }
    bool is_muted() const { return m_muted; }
    void set_muted(bool muted) { m_muted = muted; }

    u64 underrun_count() const { return m_underruns.load(); }
    // Hint of how many buffers the client is ahead by (latency = buffers * AUDIO_BUFFER_SIZE / rate).
    size_t buffered() const { return m_buffer ? m_buffer->weak_used() : 0; }

private:
    OwnPtr<Audio::AudioQueue> m_buffer;
    Vector<Audio::Sample> m_current_chunk;
    size_t m_in_chunk_location { 0 };
    bool m_paused { true };
    bool m_muted { false };
    u32 m_sample_rate { 0 };
    double m_volume { 1 };
    Atomic<u64> m_underruns { 0 };
};

}
