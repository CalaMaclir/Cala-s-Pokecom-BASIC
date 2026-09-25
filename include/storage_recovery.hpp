#pragma once
#include "program_store.hpp"

namespace rmb::storage_recovery {
enum class ProgramAction {
    None,
    UseInternalRam,
    RecoverSession,
    AskDirtyRam
};
inline bool reload_after_remount(bool remount_succeeded) {
    return remount_succeeded;
}
inline ProgramAction program_action(
    bool remount_succeeded,
    ProgramStorageMode configured_mode,
    ProgramBackend current_backend,
    bool current_dirty
) {
    if (!remount_succeeded) return ProgramAction::None;
    if (configured_mode == ProgramStorageMode::InternalRam)
        return ProgramAction::UseInternalRam;
    if (current_backend == ProgramBackend::Ram && current_dirty)
        return ProgramAction::AskDirtyRam;
    return ProgramAction::RecoverSession;
}
} // namespace rmb::storage_recovery
