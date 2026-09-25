#include "bluetooth_serial_core.hpp"

namespace rmb::bluetooth_serial {

void Core::clear_rx() {
    rx_read_ = rx_write_ = rx_count_ = 0;
}

void Core::clear_buffers() {
    clear_rx();
    tx_read_ = tx_write_ = tx_count_ = 0;
}

void Core::enable() {
    clear_buffers();
    rx_overflow_ = tx_overflow_ = 0;
    tx_queued_bytes_ = tx_sent_bytes_ = 0;
    console_enabled_ = test_terminal_active_ = transfer_active_ = false;
    enabled_ = true;
    connected_ = false;
    state_ = State::Ready;
}

void Core::disable() {
    clear_buffers();
    console_enabled_ = test_terminal_active_ = transfer_active_ = false;
    enabled_ = connected_ = false;
    state_ = State::Off;
}

void Core::set_discoverable() {
    if (enabled_ && !connected_) state_ = State::Discoverable;
}

void Core::set_connected(bool value) {
    if (!enabled_) return;
    clear_buffers();
    transfer_active_ = false;
    connected_ = value;
    state_ = value ? State::Connected : State::Discoverable;
}

void Core::set_error() {
    clear_buffers();
    transfer_active_ = false;
    connected_ = false;
    state_ = enabled_ ? State::Error : State::Off;
}

std::size_t Core::enqueue_rx(const std::uint8_t* data, std::size_t length) {
    if (!enabled_ || !connected_ || !data) return 0;
    std::size_t n = 0;
    while (n < length && rx_count_ < kRxCapacity) {
        rx_[rx_write_] = data[n++];
        rx_write_ = (rx_write_ + 1u) % kRxCapacity;
        ++rx_count_;
    }
    rx_overflow_ += static_cast<std::uint32_t>(length - n);
    return n;
}

bool Core::set_console_enabled(bool value) {
    if (value && !enabled_) return false;
    if (console_enabled_ != value) clear_rx();
    console_enabled_ = value;
    return true;
}

void Core::set_test_terminal_active(bool value) {
    if (value && transfer_active_) return;
    if (test_terminal_active_ != value) clear_rx();
    test_terminal_active_ = enabled_ && value;
}

bool Core::begin_transfer() {
    if (!enabled_ || !connected_ ||
        test_terminal_active_ || transfer_active_) return false;
    clear_rx();
    transfer_active_ = true;
    return true;
}

void Core::end_transfer() {
    if (!transfer_active_) return;
    clear_rx();
    transfer_active_ = false;
}

RxOwner Core::rx_owner() const {
    if (!enabled_ || !connected_) return RxOwner::None;
    if (transfer_active_) return RxOwner::Transfer;
    if (test_terminal_active_) return RxOwner::TestTerminal;
    return console_enabled_ ? RxOwner::Console : RxOwner::None;
}

namespace {
std::size_t read_owned(std::uint8_t* data, std::size_t capacity,
                       std::size_t& read_pos, std::size_t& count,
                       std::uint8_t* destination,
                       std::size_t destination_capacity) {
    if (!destination || !destination_capacity) return 0;
    const std::size_t amount =
        count < destination_capacity ? count : destination_capacity;
    for (std::size_t i = 0; i < amount; ++i) {
        destination[i] = data[read_pos];
        read_pos = (read_pos + 1u) % capacity;
    }
    count -= amount;
    return amount;
}

int read_owned_byte(std::uint8_t* data, std::size_t capacity,
                    std::size_t& read_pos, std::size_t& count) {
    std::uint8_t value = 0;
    return read_owned(data, capacity, read_pos, count, &value, 1) == 1
        ? static_cast<int>(value) : -1;
}
}

int Core::read_console_rx() {
    return rx_owner() == RxOwner::Console
        ? read_owned_byte(rx_, kRxCapacity, rx_read_, rx_count_) : -1;
}

int Core::read_test_rx() {
    return rx_owner() == RxOwner::TestTerminal
        ? read_owned_byte(rx_, kRxCapacity, rx_read_, rx_count_) : -1;
}

int Core::read_transfer_rx() {
    std::uint8_t value = 0;
    return read_transfer_rx(&value, 1) == 1
        ? static_cast<int>(value) : -1;
}

std::size_t Core::read_transfer_rx(std::uint8_t* destination,
                                   std::size_t capacity) {
    return rx_owner() == RxOwner::Transfer
        ? read_owned(rx_, kRxCapacity, rx_read_, rx_count_,
                     destination, capacity)
        : 0;
}

std::size_t Core::enqueue_tx(const std::uint8_t* data, std::size_t length) {
    if (!enabled_ || !connected_ || !data) return 0;
    std::size_t n = 0;
    while (n < length && tx_count_ < kTxCapacity) {
        tx_[tx_write_] = data[n++];
        tx_write_ = (tx_write_ + 1u) % kTxCapacity;
        ++tx_count_;
    }
    tx_queued_bytes_ += static_cast<std::uint32_t>(n);
    tx_overflow_ += static_cast<std::uint32_t>(length - n);
    return n;
}

bool Core::enqueue_tx_exact(const std::uint8_t* data, std::size_t length) {
    if (!enabled_ || !connected_ || !data || length > tx_free()) return false;
    return enqueue_tx(data, length) == length;
}

std::size_t Core::peek_tx(std::uint8_t* output, std::size_t capacity) const {
    if (!output || !capacity) return 0;
    const std::size_t n = tx_count_ < capacity ? tx_count_ : capacity;
    std::size_t cursor = tx_read_;
    for (std::size_t i = 0; i < n; ++i) {
        output[i] = tx_[cursor];
        cursor = (cursor + 1u) % kTxCapacity;
    }
    return n;
}

void Core::complete_tx_send(std::size_t length) {
    if (length > tx_count_) length = tx_count_;
    tx_read_ = (tx_read_ + length) % kTxCapacity;
    tx_count_ -= length;
    tx_sent_bytes_ += static_cast<std::uint32_t>(length);
}

} // namespace rmb::bluetooth_serial
