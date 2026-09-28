#pragma once

#include <cstddef>

namespace rmb::file_management {

enum class Access {
    Allowed,
    UsbHost,
    Busy,
    Unavailable
};

enum class Result {
    Success,
    InvalidName,
    Protected,
    AlreadyExists,
    NotFound,
    NotRegularFile,
    UsbHost,
    Busy,
    Unavailable,
    IoError
};

bool protected_name(const char* name, const char* current_file = nullptr);
bool normalize_program_name(
    const char* input,
    char* output,
    std::size_t capacity
);
Result rename_file(
    const char* root,
    const char* old_name,
    const char* new_name,
    const char* current_file,
    Access access = Access::Allowed,
    bool recovery = false
);
Result delete_file(
    const char* root,
    const char* name,
    const char* current_file,
    Access access = Access::Allowed
);
const char* message(Result result);

} // namespace rmb::file_management
