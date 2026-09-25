#include "bluetooth_serial_core.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using rmb::bluetooth_serial::Core;
using rmb::bluetooth_serial::RxOwner;
using rmb::bluetooth_serial::State;
using rmb::bluetooth_serial::TxRequestState;

int main() {
    Core core;
    assert(!core.enabled() && core.state() == State::Off);
    assert(!core.set_console_enabled(true));

    core.enable();
    core.set_discoverable();
    core.set_connected(true);
    assert(core.set_console_enabled(true));
    assert(core.rx_owner() == RxOwner::Console);

    const std::array<std::uint8_t, 12> binary = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x06,
        0x15, 0x18, 0x1a, 0x7f, 0x80, 0xff
    };
    assert(core.enqueue_rx(binary.data(), binary.size()) == binary.size());
    assert(core.begin_transfer());
    assert(core.rx_owner() == RxOwner::Transfer);
    assert(core.read_console_rx() == -1 && core.read_test_rx() == -1);
    core.set_test_terminal_active(true);
    assert(!core.test_terminal_active()); // Transfer ownership is exclusive.

    assert(core.enqueue_rx(binary.data(), binary.size()) == binary.size());
    for (std::uint8_t value : binary)
        assert(core.read_transfer_rx() == static_cast<int>(value));
    assert(core.read_transfer_rx() == -1);

    // Exercise the actual Bluetooth RX ring with XMODEM and YMODEM-sized
    // bursts. The byte pattern covers the complete binary range, including
    // Ctrl+C, without any text or BREAK interpretation in Transfer mode.
    auto test_rx_burst = [&core](std::size_t length) {
        std::vector<std::uint8_t> burst(length);
        for (std::size_t i = 0; i < length; ++i)
            burst[i] = static_cast<std::uint8_t>(i & 0xffu);
        const std::uint32_t overflow_before = core.rx_overflow_count();
        assert(core.enqueue_rx(burst.data(), burst.size()) == burst.size());
        assert(core.rx_size() == burst.size());
        assert(core.rx_overflow_count() == overflow_before);

        std::vector<std::uint8_t> received;
        std::array<std::uint8_t, 512> chunk = {};
        while (received.size() < burst.size()) {
            const auto count =
                core.read_transfer_rx(chunk.data(), chunk.size());
            assert(count > 0 && count <= chunk.size());
            received.insert(
                received.end(), chunk.begin(), chunk.begin() + count);
        }
        assert(received == burst);
        assert(core.read_transfer_rx(chunk.data(), chunk.size()) == 0);
        assert(core.read_transfer_rx() == -1);
    };
    test_rx_burst(128);
    test_rx_burst(1029);
    test_rx_burst(2048);

    // A 4110-byte BASIC file occupies five 1K YMODEM frames. The complete
    // five-frame burst must fit even if RFCOMM briefly outruns the protocol
    // consumer. The former 4096-byte ring could not hold this workload.
    {
        constexpr std::size_t transfer_burst = 5 * 1029;
        std::vector<std::uint8_t> burst(transfer_burst);
        for (std::size_t i = 0; i < burst.size(); ++i)
            burst[i] = static_cast<std::uint8_t>((i * 29u) & 0xffu);
        const auto overflow_start = core.rx_overflow_count();
        assert(core.enqueue_rx(burst.data(), burst.size()) == burst.size());
        assert(core.rx_overflow_count() == overflow_start);

        std::vector<std::uint8_t> received;
        std::array<std::uint8_t, 2048> chunk = {};
        while (core.rx_size()) {
            const auto count =
                core.read_transfer_rx(chunk.data(), chunk.size());
            assert(count > 0);
            received.insert(
                received.end(), chunk.begin(), chunk.begin() + count);
        }
        assert(received == burst);
    }

    // Sustained receive: enqueue and bulk-drain five consecutive YMODEM
    // 1K-frame-sized bursts without allowing the 4096-byte ring to overflow.
    {
        std::vector<std::uint8_t> expected;
        std::vector<std::uint8_t> received;
        std::array<std::uint8_t, 512> chunk = {};
        const auto overflow_start = core.rx_overflow_count();
        for (std::size_t packet = 0; packet < 5; ++packet) {
            std::vector<std::uint8_t> burst(1029);
            for (std::size_t i = 0; i < burst.size(); ++i)
                burst[i] = static_cast<std::uint8_t>(
                    (packet * 53u + i) & 0xffu);
            expected.insert(expected.end(), burst.begin(), burst.end());
            assert(core.enqueue_rx(burst.data(), burst.size()) ==
                   burst.size());
            while (core.rx_size()) {
                const auto count =
                    core.read_transfer_rx(chunk.data(), chunk.size());
                assert(count > 0);
                received.insert(
                    received.end(), chunk.begin(), chunk.begin() + count);
            }
        }
        assert(received == expected);
        assert(core.rx_overflow_count() == overflow_start);
    }

    // Capacity boundary remains exact: a full ring succeeds, then one
    // additional byte is rejected and counted as one overflow.
    std::vector<std::uint8_t> full_ring(Core::kRxCapacity);
    for (std::size_t i = 0; i < full_ring.size(); ++i)
        full_ring[i] = static_cast<std::uint8_t>((i * 37u) & 0xffu);
    const std::uint32_t overflow_before = core.rx_overflow_count();
    assert(core.enqueue_rx(full_ring.data(), full_ring.size()) ==
           full_ring.size());
    assert(core.rx_size() == Core::kRxCapacity);
    const std::uint8_t extra = 0xa5;
    assert(core.enqueue_rx(&extra, 1) == 0);
    assert(core.rx_overflow_count() == overflow_before + 1);
    for (std::uint8_t value : full_ring)
        assert(core.read_transfer_rx() == static_cast<int>(value));
    assert(core.read_transfer_rx() == -1);

    // Transfer protocol bytes must not leak back to Console after release.
    const std::uint8_t stale = 0x03;
    assert(core.enqueue_rx(&stale, 1) == 1);
    core.end_transfer();
    assert(core.rx_owner() == RxOwner::Console);
    assert(core.read_transfer_rx() == -1);
    assert(core.begin_transfer());
    assert(core.read_transfer_rx() == -1);
    core.end_transfer();

    // File Transfer may own SPP while Bluetooth Console is OFF.
    assert(core.set_console_enabled(false));
    assert(core.rx_owner() == RxOwner::None);
    assert(core.begin_transfer());
    assert(core.rx_owner() == RxOwner::Transfer);
    core.end_transfer();
    assert(core.rx_owner() == RxOwner::None);
    assert(core.set_console_enabled(true));

    const std::uint8_t a = 'A';
    core.set_test_terminal_active(true);
    assert(core.rx_owner() == RxOwner::TestTerminal);
    assert(core.enqueue_rx(&a, 1) == 1);
    assert(core.read_test_rx() == 'A' && core.read_console_rx() == -1);
    assert(!core.begin_transfer());
    core.set_test_terminal_active(false);
    assert(core.rx_owner() == RxOwner::Console);

    std::array<std::uint8_t, Core::kTxCapacity> tx = {};
    assert(core.enqueue_tx_exact(tx.data(), tx.size()));
    assert(core.tx_free() == 0);
    const std::size_t before = core.tx_size();
    assert(!core.enqueue_tx_exact(&a, 1));
    assert(core.tx_size() == before); // Exact enqueue never partially mutates.
    core.complete_tx_send(73);
    assert(core.tx_size() == Core::kTxCapacity - 73);
    assert(core.enqueue_tx_exact(binary.data(), binary.size()));
    assert(core.tx_size() == Core::kTxCapacity - 73 + binary.size());

    TxRequestState scheduler;
    assert(scheduler.begin(true, 1) && scheduler.pending());
    assert(!scheduler.begin(true, 1));
    scheduler.on_can_send_now();
    assert(scheduler.begin(true, 1));
    scheduler.on_request_failed();
    assert(!scheduler.pending());

    assert(core.begin_transfer());
    assert(core.enqueue_rx(&stale, 1) == 1);
    core.set_connected(false);
    assert(core.rx_owner() == RxOwner::None);
    assert(!core.transfer_active());
    assert(core.console_enabled() && core.tx_size() == 0);
    core.set_connected(true);
    assert(core.rx_owner() == RxOwner::Console);
    assert(core.begin_transfer());
    assert(core.read_transfer_rx() == -1);
    core.end_transfer(); // Reconnect can acquire Transfer again.

    core.disable();
    assert(!core.enabled() && !core.console_enabled());
    assert(!core.transfer_active() && core.state() == State::Off);
    std::puts(
        "Bluetooth RX bursts, ownership, binary and TX safety tests passed"
    );
}
