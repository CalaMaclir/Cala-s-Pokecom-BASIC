#include "xmodem.hpp"
#include <cstring>

namespace rmb::xmodem {
namespace {
constexpr std::uint8_t SOH = 1, EOT = 4, ACK = 6, NAK = 21, CAN = 24;
constexpr unsigned retries = 10, startup_retries = 30;
bool control(IO& io, std::uint8_t b) { return io.write(io.context, &b, 1); }
Error input_error(int c) {
    return c == cancelled || c == CAN ? Error::Cancelled :
           c == disconnected ? Error::Disconnected : Error::Timeout;
}
Result fail(IO& io, Error error, std::uint32_t bytes) {
    const std::uint8_t abort[] = {CAN, CAN, CAN};
    io.write(io.context, abort, sizeof(abort));
    return {error, bytes};
}
// Drain a damaged partial packet until a short quiet interval (bounded even
// for a continuous noise source). Do not mistake CAN inside data for cancel.
int drain(IO& io) {
    for (unsigned i = 0; i < 2048; ++i) {
        int c = io.read(io.context, 100);
        if (c < 0) return c;
    }
    return timeout;
}
}
std::uint16_t crc16(const std::uint8_t* data, std::size_t size) {
    std::uint16_t crc = 0;
    while (size--) {
        crc ^= static_cast<std::uint16_t>(*data++) << 8;
        for (int i = 0; i < 8; ++i)
            crc = static_cast<std::uint16_t>((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}
Result receive(IO& io) {
    std::uint8_t expected = 1;
    std::uint32_t bytes = 0;
    unsigned errors = 0, waiting = 0;
    bool started = false;
    Error last = Error::Timeout;
    if (!control(io, 'C')) return {Error::Disconnected, 0};
    while (true) {
        int c = io.read(io.context, started ? 3000 : 2000);
        if (c == cancelled || c == disconnected || c == CAN)
            return fail(io, input_error(c), bytes);
        if (c == EOT) {
            if (!io.finish(io.context)) return fail(io, Error::Write, bytes);
            // File is already committed. ACK retransmitted EOTs if the first
            // ACK was lost; never append or commit the file a second time.
            control(io, ACK);
            for (unsigned i = 0; i < 10; ++i) {
                int tail = io.read(io.context, 3000);
                if (tail == EOT) control(io, ACK);
                else if (tail < 0) break;
            }
            return {Error::None, bytes};
        }
        if (c == SOH) {
            std::uint8_t packet[132]; // seq, ~seq, 128 bytes, CRC big endian
            bool complete = true;
            for (auto& b : packet) {
                int v = io.read(io.context, 1000);
                if (v == cancelled || v == disconnected)
                    return fail(io, input_error(v), bytes);
                if (v < 0) { complete = false; break; }
                b = static_cast<std::uint8_t>(v);
            }
            if (!complete) {
                last = Error::Timeout;
                int d = drain(io);
                if (d == cancelled || d == disconnected) return fail(io, input_error(d), bytes);
            } else if (static_cast<std::uint8_t>(packet[0] ^ packet[1]) != 255) {
                last = Error::Protocol;
            } else if (crc16(packet + 2, 128) !=
                       static_cast<std::uint16_t>((packet[130] << 8) | packet[131])) {
                last = Error::Crc;
            } else if (packet[0] == expected) {
                if (!io.sink(io.context, packet + 2, 128)) return fail(io, Error::Write, bytes);
                bytes += 128;
                ++expected; // modulo 256, including the 255 -> 0 boundary
                started = true;
                errors = 0;
                if (!control(io, ACK)) return {Error::Disconnected, bytes};
                continue;
            } else if (started && packet[0] == static_cast<std::uint8_t>(expected - 1)) {
                // Lost ACK: acknowledge duplicate without writing twice.
                if (++errors >= retries) return fail(io, Error::Protocol, bytes);
                control(io, ACK);
                continue;
            } else {
                return fail(io, Error::Protocol, bytes);
            }
        } else if (!started && (c == '\r' || c == '\n') && waiting++ < startup_retries) {
            continue; // optional CRLF tail of the XRECV command, not a packet
        } else if (c >= 0) {
            last = Error::Protocol;
            int d = drain(io);
            if (d == cancelled || d == disconnected) return fail(io, input_error(d), bytes);
        } else {
            last = Error::Timeout;
        }
        if (!started && c < 0) {
            if (++waiting >= startup_retries) return fail(io, last, bytes);
        } else if (++errors >= retries) {
            return fail(io, last, bytes);
        }
        if (!control(io, (started || c == SOH) ? NAK : 'C')) return {Error::Disconnected, bytes};
    }
}
Result send(IO& io) {
    bool ready = false;
    for (unsigned i = 0; i < startup_retries; ++i) {
        int c = io.read(io.context, 2000);
        if (c == 'C') { ready = true; break; }
        if (c == CAN || c == cancelled || c == disconnected) return fail(io, input_error(c), 0);
        if (c == NAK) return fail(io, Error::Protocol, 0); // CRC only
    }
    if (!ready) return fail(io, Error::Timeout, 0);
    std::uint8_t seq = 1;
    std::uint32_t bytes = 0;
    while (true) {
        std::uint8_t packet[133];
        std::memset(packet + 3, 0x1a, 128);
        int n = io.source(io.context, packet + 3, 128);
        if (n < 0 || n > 128) return fail(io, Error::Read, bytes);
        if (n == 0) break;
        packet[0] = SOH; packet[1] = seq; packet[2] = static_cast<std::uint8_t>(~seq);
        auto crc = crc16(packet + 3, 128);
        packet[131] = crc >> 8; packet[132] = crc & 255;
        bool accepted = false;
        Error last = Error::Timeout;
        for (unsigned i = 0; i < retries; ++i) {
            if (!io.write(io.context, packet, sizeof(packet))) return {Error::Disconnected, bytes};
            int c = io.read(io.context, 3000);
            if (c == ACK) { accepted = true; break; }
            if (c == CAN || c == cancelled || c == disconnected) return fail(io, input_error(c), bytes);
            last = c == NAK ? Error::Crc : c < 0 ? Error::Timeout : Error::Protocol;
        }
        if (!accepted) return fail(io, last, bytes);
        bytes += static_cast<unsigned>(n);
        ++seq;
    }
    for (unsigned i = 0; i < retries; ++i) {
        if (!control(io, EOT)) return {Error::Disconnected, bytes};
        int c = io.read(io.context, 3000);
        if (c == ACK) return {Error::None, bytes};
        if (c == CAN || c == cancelled || c == disconnected) return fail(io, input_error(c), bytes);
        // NAK then EOT then ACK is also accepted.
    }
    return fail(io, Error::Timeout, bytes);
}
const char* error_text(Error e) {
    switch (e) {
        case Error::None: return "TRANSFER COMPLETE";
        case Error::Timeout: return "TIMEOUT";
        case Error::Cancelled: return "CANCELLED";
        case Error::Disconnected: return "USB DISCONNECTED";
        case Error::Crc: return "CRC ERROR";
        case Error::Read: return "SD READ ERROR";
        case Error::Write: return "SD WRITE ERROR";
        default: return "PROTOCOL ERROR (USE CRC / 128 BYTE)";
    }
}
}
