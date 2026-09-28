#include "file_management.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "program_file_guard.hpp"
#include "safe_file.hpp"

namespace rmb::file_management {
namespace {

Result access_result(Access access) {
    switch (access) {
    case Access::Allowed: return Result::Success;
    case Access::UsbHost: return Result::UsbHost;
    case Access::Busy: return Result::Busy;
    case Access::Unavailable: return Result::Unavailable;
    }
    return Result::Unavailable;
}

bool path_for(const char* root, const char* name, char* output, std::size_t capacity) {
    if (!root || !SafeFileWriter::valid_root_name(name)) return false;
    return std::snprintf(output, capacity, "%s%s", root, name) <
        static_cast<int>(capacity);
}

Result inspect_regular(const char* path) {
    struct stat value = {};
    if (stat(path, &value) != 0) {
        return errno == ENOENT ? Result::NotFound : Result::IoError;
    }
    return S_ISREG(value.st_mode) ? Result::Success : Result::NotRegularFile;
}

bool intrinsically_protected(const char* name) {
    return program_files::reserved(name) ||
        program_files::transfer_staging(name) ||
        program_files::session_staging(name) ||
        program_files::equal(name, "RMBASIC.SES") ||
        program_files::equal(name, "RMBASIC.CFG") ||
        program_files::equal(name, "RMBASIC.BAK");
}

} // namespace

bool normalize_program_name(
    const char* input,
    char* output,
    std::size_t capacity
) {
    if (!output || capacity == 0) return false;
    output[0] = '\0';
    if (!SafeFileWriter::valid_root_name(input)) return false;

    const std::size_t length = std::strlen(input);
    const bool has_bas = length >= 4 &&
        program_files::equal(input + length - 4, ".BAS");
    const std::size_t required = length + (has_bas ? 0u : 4u) + 1u;
    if (required > capacity || required > 80u) return false;

    std::snprintf(output, capacity, "%s%s", input, has_bas ? "" : ".BAS");
    return SafeFileWriter::valid_root_name(output);
}

bool protected_name(const char* name, const char* current_file) {
    if (!name || !*name) return false;
    return intrinsically_protected(name) ||
        (current_file && *current_file &&
         !program_files::equal(current_file, "UNTITLED") &&
         program_files::equal(name, current_file)) ||
        program_files::in_use(name);
}

Result rename_file(
    const char* root,
    const char* old_name,
    const char* new_name,
    const char* current_file,
    Access access,
    bool recovery
) {
    const Result allowed = access_result(access);
    if (allowed != Result::Success) return allowed;
    if (!SafeFileWriter::valid_root_name(old_name) ||
        !SafeFileWriter::valid_root_name(new_name)) return Result::InvalidName;
    // A loaded named source may be renamed: ProgramStore edits an independent
    // RMBP work file. Keep the destination protected when it names some other
    // loaded source, and always protect internal/staging/in-use files.
    const bool source_is_current = current_file && *current_file &&
        program_files::equal(old_name, current_file);
    const bool destination_is_current = current_file && *current_file &&
        program_files::equal(new_name, current_file);
    const bool source_protected =
        intrinsically_protected(old_name) ||
        (!source_is_current && protected_name(old_name, current_file));
    const bool destination_protected =
        intrinsically_protected(new_name) ||
        (!(recovery && destination_is_current) &&
         protected_name(new_name, current_file));
    if (source_protected || destination_protected) {
        return Result::Protected;
    }
    if (program_files::equal(old_name, new_name)) return Result::AlreadyExists;

    char old_path[192] = {};
    char new_path[192] = {};
    if (!path_for(root, old_name, old_path, sizeof(old_path)) ||
        !path_for(root, new_name, new_path, sizeof(new_path))) {
        return Result::InvalidName;
    }
    const Result source = inspect_regular(old_path);
    if (source != Result::Success) return source;

    struct stat value = {};
    if (stat(new_path, &value) == 0) return Result::AlreadyExists;
    if (errno != ENOENT) return Result::IoError;
    return std::rename(old_path, new_path) == 0
        ? Result::Success : Result::IoError;
}

Result delete_file(
    const char* root,
    const char* name,
    const char* current_file,
    Access access
) {
    const Result allowed = access_result(access);
    if (allowed != Result::Success) return allowed;
    if (!SafeFileWriter::valid_root_name(name)) return Result::InvalidName;
    if (protected_name(name, current_file)) return Result::Protected;

    char path[192] = {};
    if (!path_for(root, name, path, sizeof(path))) return Result::InvalidName;
    const Result source = inspect_regular(path);
    if (source != Result::Success) return source;
    return std::remove(path) == 0 ? Result::Success : Result::IoError;
}

const char* message(Result result) {
    switch (result) {
    case Result::Success: return "OK";
    case Result::InvalidName: return "BAD FILENAME";
    case Result::Protected: return "PROTECTED FILE";
    case Result::AlreadyExists: return "FILE EXISTS";
    case Result::NotFound: return "FILE NOT FOUND";
    case Result::NotRegularFile: return "NOT A REGULAR FILE";
    case Result::UsbHost: return "SD CARD OWNED BY USB HOST";
    case Result::Busy: return "STORAGE BUSY";
    case Result::Unavailable: return "SD CARD NOT AVAILABLE";
    case Result::IoError: return "SD FILE OPERATION FAILED";
    }
    return "SD FILE OPERATION FAILED";
}

} // namespace rmb::file_management
