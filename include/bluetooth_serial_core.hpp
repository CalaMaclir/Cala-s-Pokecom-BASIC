#pragma once
#include <cstddef>
#include <cstdint>
namespace rmb::bluetooth_serial {
enum class State : std::uint8_t { Off, Ready, Discoverable, Connected, Error };
enum class RxOwner : std::uint8_t { None, Console, TestTerminal, Transfer };

class TxRequestState {
public:
    bool begin(bool transport_ready, std::size_t queued_bytes) {
        if (!transport_ready || queued_bytes == 0 || pending_) return false;
        // Set this before calling BTstack: CAN_SEND_NOW may be delivered synchronously.
        pending_ = true;
        return true;
    }
    void on_can_send_now() { pending_ = false; }
    void on_request_failed() { pending_ = false; }
    void reset() { pending_ = false; }
    bool pending() const { return pending_; }
private:
    bool pending_ = false;
};

class Core {
public:
    // YMODEM over RFCOMM can arrive in multi-frame bursts. Keep enough
    // headroom for a complete 4 KiB-class file transaction plus framing,
    // while still leaving ample RP2350 SRAM for lwIP/BTstack runtime use.
    static constexpr std::size_t kRxCapacity = 8192;
    static constexpr std::size_t kTxCapacity = 2048;
    void enable(); void disable();
    bool enabled() const { return enabled_; }
    bool connected() const { return connected_; }
    State state() const { return state_; }
    void set_discoverable(); void set_connected(bool connected); void set_error();
    std::size_t enqueue_rx(const std::uint8_t* data, std::size_t length);
    bool set_console_enabled(bool enabled);
    bool console_enabled() const { return console_enabled_; }
    void set_test_terminal_active(bool active);
    bool test_terminal_active() const { return test_terminal_active_; }
    bool begin_transfer();
    void end_transfer();
    bool transfer_active() const { return transfer_active_; }
    RxOwner rx_owner() const;
    int read_console_rx();
    int read_test_rx();
    int read_transfer_rx();
    std::size_t read_transfer_rx(std::uint8_t* destination,
                                 std::size_t capacity);
    std::size_t enqueue_tx(const std::uint8_t* data, std::size_t length);
    bool enqueue_tx_exact(const std::uint8_t* data, std::size_t length);
    std::size_t peek_tx(std::uint8_t* output, std::size_t capacity) const;
    void complete_tx_send(std::size_t length);
    std::size_t rx_size() const { return rx_count_; }
    std::size_t tx_size() const { return tx_count_; }
    std::size_t tx_free() const { return kTxCapacity - tx_count_; }
    void record_tx_drop(std::size_t length) {
        tx_overflow_ += static_cast<std::uint32_t>(length);
    }
    std::uint32_t rx_overflow_count() const { return rx_overflow_; }
    std::uint32_t tx_overflow_count() const { return tx_overflow_; }
    std::uint32_t tx_queued_bytes() const { return tx_queued_bytes_; }
    std::uint32_t tx_sent_bytes() const { return tx_sent_bytes_; }
private:
    void clear_buffers();
    void clear_rx();
    std::uint8_t rx_[kRxCapacity] = {};
    std::uint8_t tx_[kTxCapacity] = {};
    std::size_t rx_read_=0, rx_write_=0, rx_count_=0;
    std::size_t tx_read_=0, tx_write_=0, tx_count_=0;
    std::uint32_t rx_overflow_=0, tx_overflow_=0;
    std::uint32_t tx_queued_bytes_=0, tx_sent_bytes_=0;
    bool enabled_=false, connected_=false;
    bool console_enabled_=false, test_terminal_active_=false, transfer_active_=false;
    State state_=State::Off;
};
} // namespace rmb::bluetooth_serial
