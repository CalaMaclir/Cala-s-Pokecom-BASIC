#pragma once

#include <cstddef>

namespace rmb::audio {

inline bool ascii_equal_ci(char left, char right) {
    if (left >= 'a' && left <= 'z') left -= 'a' - 'A';
    if (right >= 'a' && right <= 'z') right -= 'a' - 'A';
    return left == right;
}

inline bool has_extension_ci(const char* filename, const char* extension) {
    if (!filename || !extension) return false;
    std::size_t name_length = 0;
    std::size_t extension_length = 0;
    while (filename[name_length]) ++name_length;
    while (extension[extension_length]) ++extension_length;
    if (name_length < extension_length) return false;
    const char* suffix = filename + name_length - extension_length;
    for (std::size_t i = 0; i < extension_length; ++i) {
        if (!ascii_equal_ci(suffix[i], extension[i])) return false;
    }
    return true;
}

inline bool playable_audio_filename(const char* filename) {
    return has_extension_ci(filename, ".WAV") ||
           has_extension_ci(filename, ".MP3");
}

inline bool wav_file_header(const unsigned char* header, std::size_t size) {
    return header && size >= 12 &&
        header[0] == 'R' && header[1] == 'I' &&
        header[2] == 'F' && header[3] == 'F' &&
        header[8] == 'W' && header[9] == 'A' &&
        header[10] == 'V' && header[11] == 'E';
}

} // namespace rmb::audio
