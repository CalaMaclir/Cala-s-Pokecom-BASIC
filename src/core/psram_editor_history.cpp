#include "psram_editor_history.hpp"

#include <algorithm>
#include <cstring>

#include "psram.hpp"

namespace rmb {
namespace {

constexpr std::uint32_t kHistoryBytes = 128u * 1024u;
constexpr std::uint32_t kRecordMagic = 0x554e4431u; // UND1

struct StoredRecord {
    std::uint32_t magic = kRecordMagic;
    EditorHistorySnapshot snapshot{};
    std::uint32_t crc32 = 0;
};

std::uint32_t crc32_update(
    std::uint32_t crc,
    const void* source,
    std::size_t length
) {
    const auto* data = static_cast<const std::uint8_t*>(source);
    while (length-- != 0) {
        crc ^= *data++;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

std::uint32_t snapshot_crc(const EditorHistorySnapshot& snapshot) {
    std::uint32_t crc = 0xffffffffu;
    crc = crc32_update(crc, &snapshot.state_id, sizeof(snapshot.state_id));
    crc = crc32_update(crc, &snapshot.line_number, sizeof(snapshot.line_number));
    crc = crc32_update(crc, &snapshot.line_index, sizeof(snapshot.line_index));
    crc = crc32_update(crc, &snapshot.length, sizeof(snapshot.length));
    crc = crc32_update(crc, &snapshot.cursor, sizeof(snapshot.cursor));
    const std::uint8_t existed = snapshot.existed ? 1u : 0u;
    crc = crc32_update(crc, &existed, sizeof(existed));
    crc = crc32_update(crc, snapshot.body, snapshot.length + 1u);
    return crc ^ 0xffffffffu;
}

} // namespace

PsramEditorHistory::~PsramEditorHistory() {
    end();
}

bool PsramEditorHistory::begin() {
    end();
    std::uint32_t base = 0;
    std::uint32_t bytes = 0;
    if (!psram::claim(
            psram::Client::EditorHistory,
            kHistoryBytes,
            base,
            bytes)) return false;
    const std::uint32_t records = bytes / sizeof(StoredRecord);
    const std::uint32_t per_stack = records / 2u;
    if (per_stack == 0) {
        psram::release(psram::Client::EditorHistory);
        return false;
    }
    allocated_bytes_ = per_stack * 2u * sizeof(StoredRecord);
    undo_.base = base;
    undo_.capacity = per_stack;
    redo_.base = base + per_stack * sizeof(StoredRecord);
    redo_.capacity = per_stack;
    ready_ = true;
    clear();
    return true;
}

void PsramEditorHistory::end() {
    if (ready_) psram::release(psram::Client::EditorHistory);
    ready_ = false;
    allocated_bytes_ = 0;
    undo_ = {};
    redo_ = {};
}

void PsramEditorHistory::clear() {
    undo_.start = undo_.count = 0;
    redo_.start = redo_.count = 0;
}

void PsramEditorHistory::clear_redo() {
    redo_.start = redo_.count = 0;
}

bool PsramEditorHistory::push(
    Stack& stack,
    const EditorHistorySnapshot& snapshot
) {
    if (!ready_ || stack.capacity == 0 ||
        snapshot.length >= kMaxSdProgramLineLength ||
        snapshot.cursor > snapshot.length) return false;
    StoredRecord record{};
    record.snapshot = snapshot;
    record.crc32 = snapshot_crc(record.snapshot);
    std::uint32_t index = 0;
    if (stack.count < stack.capacity) {
        index = (stack.start + stack.count) % stack.capacity;
        ++stack.count;
    } else {
        index = stack.start;
        stack.start = (stack.start + 1u) % stack.capacity;
    }
    return psram::write(
        stack.base + index * sizeof(StoredRecord),
        &record,
        sizeof(record));
}

bool PsramEditorHistory::pop(
    Stack& stack,
    EditorHistorySnapshot& snapshot
) {
    if (!ready_ || stack.count == 0 || stack.capacity == 0) return false;
    const std::uint32_t index =
        (stack.start + stack.count - 1u) % stack.capacity;
    StoredRecord record{};
    if (!psram::read(
            stack.base + index * sizeof(StoredRecord),
            &record,
            sizeof(record))) return false;
    if (record.magic != kRecordMagic ||
        record.snapshot.length >= kMaxSdProgramLineLength ||
        record.snapshot.cursor > record.snapshot.length ||
        record.snapshot.body[record.snapshot.length] != '\0' ||
        record.crc32 != snapshot_crc(record.snapshot)) return false;
    --stack.count;
    snapshot = record.snapshot;
    return true;
}

bool PsramEditorHistory::push_undo(
    const EditorHistorySnapshot& snapshot
) {
    return push(undo_, snapshot);
}

bool PsramEditorHistory::pop_undo(EditorHistorySnapshot& snapshot) {
    return pop(undo_, snapshot);
}

bool PsramEditorHistory::push_redo(
    const EditorHistorySnapshot& snapshot
) {
    return push(redo_, snapshot);
}

bool PsramEditorHistory::pop_redo(EditorHistorySnapshot& snapshot) {
    return pop(redo_, snapshot);
}

} // namespace rmb
