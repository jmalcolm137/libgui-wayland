/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ClientAudioStream.h"
#include <LibAudio/Resampler.h>

namespace LibSerenityAudio {

ErrorOr<Audio::Sample, ClientAudioStream::ErrorState> ClientAudioStream::get_next_sample(u32 device_sample_rate)
{
    if (m_paused)
        return ErrorState::ClientPaused;

    if (!m_buffer) {
        m_underruns.fetch_add(1);
        return ErrorState::ClientUnderrun;
    }

    if (m_in_chunk_location >= m_current_chunk.size()) {
        auto result = m_buffer->dequeue();
        if (result.is_error()) {
            m_underruns.fetch_add(1);
            return ErrorState::ClientUnderrun;
        }

        // FIXME: Our resampler is a naive insert/drop; carry state across
        // buffers and do band-corrected resampling for better quality.
        auto maybe_resampled = Audio::ResampleHelper<Audio::Sample> { m_sample_rate == 0 ? device_sample_rate : m_sample_rate, device_sample_rate }
                                   .try_resample(result.release_value());
        if (maybe_resampled.is_error())
            return ErrorState::ResamplingError;

        m_current_chunk = maybe_resampled.release_value();
        m_in_chunk_location = 0;
    }

    auto sample = m_current_chunk[m_in_chunk_location++];
    if (m_muted)
        return Audio::Sample { 0, 0 };
    sample *= static_cast<float>(m_volume);
    return sample;
}

void ClientAudioStream::clear()
{
    if (!m_buffer)
        return;
    while (!m_buffer->dequeue().is_error())
        ;
}

}
