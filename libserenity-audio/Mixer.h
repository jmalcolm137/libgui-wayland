/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibThreading/Mutex.h>

#include "ClientAudioStream.h"

namespace LibSerenityAudio {

// Mixes every connected client's stream into one output, like Serenity's
// AudioServer::Mixer.
class Mixer {
public:
    void add_stream(NonnullRefPtr<ClientAudioStream>);
    void remove_stream(ClientAudioStream*);
    Audio::Sample next_sample(u32 device_sample_rate);
    size_t stream_count();
    u64 underrun_count();
    size_t max_buffered();

private:
    Threading::Mutex m_mutex;
    Vector<NonnullRefPtr<ClientAudioStream>> m_streams;
};

// The single mixer shared by all connections and the PipeWire backend.
Mixer& the_mixer();

}
