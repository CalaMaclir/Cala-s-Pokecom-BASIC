#include "file_management.hpp"
#include "program_file_guard.hpp"

#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void write_file(const char* path, const char* text) {
    FILE* file = std::fopen(path, "wb");
    assert(file);
    assert(std::fwrite(text, 1, std::strlen(text), file) == std::strlen(text));
    assert(std::fclose(file) == 0);
}

bool exists(const char* path) {
    struct stat value = {};
    return stat(path, &value) == 0;
}

} // namespace

int main() {
    using namespace rmb::file_management;

    char root_template[] = "/tmp/cpb-file-management-XXXXXX";
    char* root_directory = mkdtemp(root_template);
    assert(root_directory);
    char root[160] = {};
    std::snprintf(root, sizeof(root), "%s/", root_directory);

    char alpha[192] = {};
    char beta[192] = {};
    char gamma[192] = {};
    char spaced_old[192] = {};
    char spaced_new[192] = {};
    char plain_old[192] = {};
    char spaced_destination[192] = {};
    char spaced_source[192] = {};
    char plain_destination[192] = {};
    std::snprintf(alpha, sizeof(alpha), "%sALPHA.BAS", root);
    std::snprintf(beta, sizeof(beta), "%sBETA.BAS", root);
    std::snprintf(gamma, sizeof(gamma), "%sGAMMA.BAS", root);
    std::snprintf(spaced_old, sizeof(spaced_old), "%sOLD NAME.BAS", root);
    std::snprintf(spaced_new, sizeof(spaced_new), "%sNEW NAME.BAS", root);
    std::snprintf(plain_old, sizeof(plain_old), "%sOLD.BAS", root);
    std::snprintf(
        spaced_destination, sizeof(spaced_destination),
        "%sNEW NAME 1.BAS", root);
    std::snprintf(
        spaced_source, sizeof(spaced_source),
        "%sOLD NAME 2.BAS", root);
    std::snprintf(
        plain_destination, sizeof(plain_destination),
        "%sNEW.BAS", root);

    write_file(alpha, "10 PRINT 1\n");
    assert(rename_file(root, "ALPHA.BAS", "BETA.BAS", nullptr) == Result::Success);
    assert(!exists(alpha) && exists(beta));

    write_file(gamma, "20 END\n");
    assert(rename_file(root, "BETA.BAS", "GAMMA.BAS", nullptr) ==
           Result::AlreadyExists);
    assert(rename_file(root, "../BETA.BAS", "NEXT.BAS", nullptr) ==
           Result::InvalidName);
    rmb::program_files::set_active("BETA.BAS");
    assert(rename_file(root, "BETA.BAS", "NEXT.BAS", "BETA.BAS") ==
           Result::Success);
    assert(rename_file(root, "NEXT.BAS", "BETA.BAS", "BETA.BAS") ==
           Result::Protected);
    assert(rename_file(
        root, "NEXT.BAS", "BETA.BAS", "BETA.BAS",
        Access::Allowed, true) == Result::Success);
    rmb::program_files::set_active(nullptr);
    assert(rename_file(root, "BETA.BAS", "NEXT.BAS", nullptr,
                       Access::UsbHost) == Result::UsbHost);
    assert(rename_file(root, "BETA.BAS", "NEXT.BAS", nullptr,
                       Access::Busy) == Result::Busy);
    assert(rename_file(root, "BETA.BAS", "NEXT.BAS", nullptr,
                       Access::Unavailable) == Result::Unavailable);
    assert(rename_file(root, "MISSING.BAS", "NEXT.BAS", nullptr) ==
           Result::NotFound);
    assert(rename_file(root, "BETA.BAS", "RMBASIC.CFG", nullptr) ==
           Result::Protected);
    assert(rename_file(root, "BETA.BAS", "../NEXT.BAS", nullptr) ==
           Result::InvalidName);
    assert(exists(beta));

    write_file(plain_old, "31 PRINT \"DESTINATION SPACE\"\n");
    assert(rename_file(root, "OLD.BAS", "NEW NAME 1.BAS", nullptr) ==
           Result::Success);
    assert(!exists(plain_old) && exists(spaced_destination));
    FILE* destination_space = std::fopen(spaced_destination, "rb");
    assert(destination_space);
    char destination_space_body[64] = {};
    assert(std::fgets(
        destination_space_body, sizeof(destination_space_body),
        destination_space));
    assert(std::fclose(destination_space) == 0);
    assert(std::strcmp(
        destination_space_body,
        "31 PRINT \"DESTINATION SPACE\"\n") == 0);

    write_file(spaced_source, "32 PRINT \"SOURCE SPACE\"\n");
    assert(rename_file(root, "OLD NAME 2.BAS", "NEW.BAS", nullptr) ==
           Result::Success);
    assert(!exists(spaced_source) && exists(plain_destination));
    FILE* source_space = std::fopen(plain_destination, "rb");
    assert(source_space);
    char source_space_body[64] = {};
    assert(std::fgets(
        source_space_body, sizeof(source_space_body), source_space));
    assert(std::fclose(source_space) == 0);
    assert(std::strcmp(
        source_space_body, "32 PRINT \"SOURCE SPACE\"\n") == 0);

    write_file(spaced_old, "30 PRINT \"SPACES\"\n");
    assert(rename_file(root, "OLD NAME.BAS", "NEW NAME.BAS", nullptr) ==
           Result::Success);
    assert(!exists(spaced_old) && exists(spaced_new));
    FILE* renamed = std::fopen(spaced_new, "rb");
    assert(renamed);
    char renamed_body[64] = {};
    assert(std::fgets(renamed_body, sizeof(renamed_body), renamed));
    assert(std::fclose(renamed) == 0);
    assert(std::strcmp(renamed_body, "30 PRINT \"SPACES\"\n") == 0);

    char normalized[80] = {};
    assert(normalize_program_name(
        "MY PROGRAM", normalized, sizeof(normalized)));
    assert(std::strcmp(normalized, "MY PROGRAM.BAS") == 0);
    assert(normalize_program_name(
        "MY PROGRAM.BAS", normalized, sizeof(normalized)));
    assert(std::strcmp(normalized, "MY PROGRAM.BAS") == 0);
    assert(!normalize_program_name(
        " MY PROGRAM", normalized, sizeof(normalized)));
    assert(!normalize_program_name(
        "MY PROGRAM ", normalized, sizeof(normalized)));
    assert(!normalize_program_name(
        "../MY PROGRAM", normalized, sizeof(normalized)));

    assert(protected_name("RMBASIC.CFG"));
    assert(protected_name("RMBASIC.SES"));
    assert(protected_name("RMBP0001.BAS"));
    assert(protected_name("XMODEM.TMP"));

    assert(delete_file(root, "GAMMA.BAS", nullptr) == Result::Success);
    assert(!exists(gamma));
    assert(delete_file(root, "BETA.BAS", "BETA.BAS") == Result::Protected);
    assert(delete_file(root, "MISSING.BAS", nullptr) == Result::NotFound);
    assert(delete_file(root, "BETA.BAS", nullptr, Access::UsbHost) ==
           Result::UsbHost);

    assert(std::remove(beta) == 0);
    assert(std::remove(spaced_new) == 0);
    assert(std::remove(spaced_destination) == 0);
    assert(std::remove(plain_destination) == 0);
    assert(rmdir(root_directory) == 0);
    std::puts("File management: PASS");
}
