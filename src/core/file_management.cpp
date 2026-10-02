#include "file_management.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include "file_path.hpp"

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
    return file_paths::physical(root, name, output, capacity);
}

Result inspect_regular(const char* path) {
    struct stat value = {};
    if (stat(path, &value) != 0) {
        return errno == ENOENT ? Result::NotFound : Result::IoError;
    }
    return S_ISREG(value.st_mode) ? Result::Success : Result::NotRegularFile;
}

bool intrinsically_protected(const char* name) {
    if (!name) return false;
    char component[80] = {};
    std::size_t n = 0;
    for (const char* p = name;; ++p) {
        if (*p && *p != '/') {
            if (n + 1 >= sizeof(component)) return true;
            component[n++] = *p;
            continue;
        }
        component[n] = '\0';
        if (program_files::reserved(component) || program_files::transfer_staging(component) ||
            program_files::session_staging(component) ||
            program_files::equal(component, "RMBASIC.SES") ||
            program_files::equal(component, "RMBASIC.CFG") ||
            program_files::equal(component, "RMBASIC.BAK")) return true;
        if (!*p) break;
        n = 0;
    }
    return false;
}

} // namespace

bool normalize_program_name(const char* input, char* output, std::size_t capacity) {
    char path[file_paths::capacity] = {};
    if (!output || !capacity || !file_paths::normalize(input, path, sizeof(path))) return false;
    const std::size_t length = std::strlen(path);
    const bool has_bas = length >= 4 && program_files::equal(path + length - 4, ".BAS");
    const std::size_t required = length + (has_bas ? 0u : 4u) + 1u;
    if (required > capacity || required > sizeof(path)) return false;
    if (!has_bas) std::memcpy(path + length, ".BAS", 5);
    std::memcpy(output, path, required);
    return true;
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
    if (!file_paths::valid_relative(old_name) ||
        !file_paths::valid_relative(new_name)) return Result::InvalidName;
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
    if (!file_paths::valid_relative(name)) return Result::InvalidName;
    if (protected_name(name, current_file)) return Result::Protected;

    char path[192] = {};
    if (!path_for(root, name, path, sizeof(path))) return Result::InvalidName;
    const Result source = inspect_regular(path);
    if (source != Result::Success) return source;
    return std::remove(path) == 0 ? Result::Success : Result::IoError;
}


Result create_directory(const char* root, const char* name, Access access) {
    const Result allowed = access_result(access);
    if (allowed != Result::Success) return allowed;
    if (!file_paths::valid_relative(name)) return Result::InvalidName;
    if (intrinsically_protected(name)) return Result::Protected;
    char path[192] = {};
    if (!path_for(root, name, path, sizeof(path))) return Result::InvalidName;
    if (mkdir(path, 0777) == 0) return Result::Success;
    return errno == EEXIST ? Result::AlreadyExists : Result::IoError;
}

Result delete_directory(const char* root, const char* name,
                        const char* current_file, Access access) {
    const Result allowed = access_result(access);
    if (allowed != Result::Success) return allowed;
    if (!file_paths::valid_relative(name)) return Result::InvalidName;
    if (intrinsically_protected(name) ||
        file_paths::same_or_child(current_file, name) ||
        file_paths::same_or_child(program_files::active, name)) return Result::Protected;
    char path[192] = {};
    if (!path_for(root, name, path, sizeof(path))) return Result::InvalidName;
    struct stat st = {};
    if (stat(path, &st) != 0) return errno == ENOENT ? Result::NotFound : Result::IoError;
    if (!S_ISDIR(st.st_mode)) return Result::NotDirectory;
    // rmdir is atomic and never recursively removes a user's directory tree.
    if (rmdir(path) == 0) return Result::Success;
    return errno == ENOTEMPTY || errno == EEXIST ? Result::DirectoryNotEmpty : Result::IoError;
}

Result rename_directory(const char* root, const char* old_name,
                        const char* new_name, const char* current_file, Access access) {
    const Result allowed = access_result(access);
    if (allowed != Result::Success) return allowed;
    if (!file_paths::valid_relative(old_name) || !file_paths::valid_relative(new_name))
        return Result::InvalidName;
    if (intrinsically_protected(old_name) || intrinsically_protected(new_name) ||
        (file_paths::same_or_child(program_files::active, old_name) &&
         !program_files::equal(program_files::active, current_file))) return Result::Protected;
    if (file_paths::same_or_child(new_name, old_name)) return Result::InvalidName;
    char remapped[80] = {};
    if (current_file && *current_file && !program_files::equal(current_file, "UNTITLED") &&
        !file_paths::relocate(current_file, old_name, new_name, remapped, sizeof(remapped)))
        return Result::InvalidName;
    char old_path[192] = {}, new_path[192] = {};
    if (!path_for(root, old_name, old_path, sizeof(old_path)) ||
        !path_for(root, new_name, new_path, sizeof(new_path))) return Result::InvalidName;
    struct stat st = {};
    if (stat(old_path, &st) != 0) return errno == ENOENT ? Result::NotFound : Result::IoError;
    if (!S_ISDIR(st.st_mode)) return Result::NotDirectory;
    if (stat(new_path, &st) == 0) return Result::AlreadyExists;
    if (errno != ENOENT) return Result::IoError;
    return std::rename(old_path, new_path) == 0 ? Result::Success : Result::IoError;
}

const char* message(Result result) {
    switch (result) {
    case Result::Success: return "OK";
    case Result::InvalidName: return "BAD FILENAME";
    case Result::Protected: return "PROTECTED FILE";
    case Result::AlreadyExists: return "FILE EXISTS";
    case Result::NotFound: return "FILE NOT FOUND";
    case Result::NotRegularFile: return "NOT A REGULAR FILE";
    case Result::NotDirectory: return "NOT A DIRECTORY";
    case Result::DirectoryNotEmpty: return "DIRECTORY NOT EMPTY";
    case Result::UsbHost: return "SD CARD OWNED BY USB HOST";
    case Result::Busy: return "STORAGE BUSY";
    case Result::Unavailable: return "SD CARD NOT AVAILABLE";
    case Result::IoError: return "SD FILE OPERATION FAILED";
    }
    return "SD FILE OPERATION FAILED";
}

} // namespace rmb::file_management
