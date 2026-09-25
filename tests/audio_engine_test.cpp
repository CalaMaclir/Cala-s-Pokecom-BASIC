#include "audio_engine.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace {

void put16(std::vector<std::uint8_t>& data, std::uint16_t value) {
    data.push_back(static_cast<std::uint8_t>(value));
    data.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put32(std::vector<std::uint8_t>& data, std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
        data.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

void fourcc(std::vector<std::uint8_t>& data, const char* text) {
    data.insert(data.end(), text, text + 4);
}

std::vector<std::uint8_t> wav(
    int bits,
    int channels,
    int rate,
    bool unknown_chunk = false,
    int format = 1
) {
    std::vector<std::uint8_t> body;
    fourcc(body, "WAVE");
    if (unknown_chunk) {
        fourcc(body, "JUNK");
        put32(body, 3);
        body.push_back(1);
        body.push_back(2);
        body.push_back(3);
        body.push_back(0);
    }
    fourcc(body, "fmt ");
    put32(body, 16);
    put16(body, static_cast<std::uint16_t>(format));
    put16(body, static_cast<std::uint16_t>(channels));
    put32(body, static_cast<std::uint32_t>(rate));
    const int align = channels * bits / 8;
    put32(body, static_cast<std::uint32_t>(rate * align));
    put16(body, static_cast<std::uint16_t>(align));
    put16(body, static_cast<std::uint16_t>(bits));
    fourcc(body, "data");
    put32(body, static_cast<std::uint32_t>(align * 2));
    for (int i = 0; i < align * 2; ++i)
        body.push_back(static_cast<std::uint8_t>(i * 17));

    std::vector<std::uint8_t> result;
    fourcc(result, "RIFF");
    put32(result, static_cast<std::uint32_t>(body.size()));
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

bool read_at(void* context, std::uint32_t offset, void* output, std::size_t size) {
    auto& data = *static_cast<std::vector<std::uint8_t>*>(context);
    if (static_cast<std::uint64_t>(offset) + size > data.size())
        return false;
    std::memcpy(output, data.data() + offset, size);
    return true;
}

} // namespace

int main() {
    using namespace rmb::audio;

    Engine engine;
    engine.init();
    assert(engine.volume() == 70);
    engine.set_volume(130);
    assert(engine.volume() == 100);
    engine.set_volume(-1);
    assert(engine.volume() == 0);
    engine.set_volume(70);

    assert(engine.start_beep(10, 100) == Result::BadFrequency);
    assert(engine.start_beep(880, 0) == Result::BadDuration);
    assert(engine.start_beep(880, 1) == Result::Ok);
    assert(engine.active());
    std::int16_t samples[256 * 2] = {};
    engine.render(samples, 256);
    assert(!engine.active());
    assert(engine.start_beep(440, 100) == Result::Ok);
    engine.stop();
    assert(!engine.active());
    assert(engine.start_beep(660, 1) == Result::Ok);
    engine.render(samples, 256);
    assert(!engine.active());

    const char* one[] = {"T120 O4 L8 C D# E- F G A B > C. R"};
    assert(engine.start_mml(one, 1) == Result::Ok);
    engine.pause();
    assert(engine.active() && engine.paused());
    std::memset(samples, 1, sizeof(samples));
    engine.render(samples, 32);
    for (int i = 0; i < 64; ++i) assert(samples[i] == 0);
    engine.resume();
    assert(!engine.paused());
    engine.stop();
    assert(!engine.active());

    const char* three[] = {
        "T160O5L8 CDEFGAB>C",
        "T160O4L8 EFGAB>CD",
        "T160O3L4 C G C G V8 R."
    };
    assert(engine.start_mml(three, 3) == Result::Ok);
    engine.render(samples, 64);
    assert(engine.active());
    engine.stop();

    const char* bad[] = {"T0 O9 L3 X"};
    assert(engine.start_mml(bad, 1) == Result::BadMml);
    const char* four[] = {"C", "E", "G", "B"};
    assert(engine.start_mml(four, 4) == Result::TooManyVoices);

    for (int rate : {11025, 22050, 44100}) {
        for (auto format : std::vector<std::pair<int, int>>{
                 {8, 1}, {16, 1}, {16, 2}}) {
            auto data = wav(format.first, format.second, rate);
            WavInfo info;
            assert(parse_wav(
                read_at, &data, static_cast<std::uint32_t>(data.size()), info
            ) == WavError::Ok);
            assert(info.bits_per_sample == format.first);
            assert(info.channels == format.second);
            assert(info.sample_rate == static_cast<std::uint32_t>(rate));
            assert(info.data_size > 0);
        }
    }

    auto with_unknown = wav(16, 2, 22050, true);
    WavInfo info;
    assert(parse_wav(
        read_at, &with_unknown,
        static_cast<std::uint32_t>(with_unknown.size()), info
    ) == WavError::Ok);

    auto invalid = wav(16, 1, 22050);
    invalid[0] = 'N';
    assert(parse_wav(
        read_at, &invalid, static_cast<std::uint32_t>(invalid.size()), info
    ) == WavError::InvalidRiff);

    auto non_pcm = wav(16, 1, 22050, false, 3);
    assert(parse_wav(
        read_at, &non_pcm, static_cast<std::uint32_t>(non_pcm.size()), info
    ) == WavError::UnsupportedFormat);

    auto truncated = wav(16, 1, 22050);
    truncated.pop_back();
    assert(parse_wav(
        read_at, &truncated,
        static_cast<std::uint32_t>(truncated.size()), info
    ) == WavError::Truncated);
}
