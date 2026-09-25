#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace rmb::audio {
inline unsigned wav_tail_gain(std::uint32_t remaining_frames,
                               std::uint32_t sample_rate) {
    const std::uint32_t tail = std::max<std::uint32_t>(1u, sample_rate / 200u);
    return remaining_frames >= tail ? 256u :
        static_cast<unsigned>(remaining_frames * 256u / tail);
}
inline bool has_pcm_samples(const std::int16_t* pcm, std::size_t count) {
    for (std::size_t i=0; i<count; ++i) if (pcm[i] != 0) return true;
    return false;
}
} // namespace rmb::audio
