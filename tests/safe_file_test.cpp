#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>
#include "safe_file.hpp"

static std::string read_all(const char* path) {
    FILE* file = std::fopen(path, "rb");
    assert(file);
    std::string value;
    char block[1024];
    std::size_t count;
    while ((count = std::fread(block, 1, sizeof(block), file)) != 0)
        value.append(block, count);
    assert(!std::ferror(file));
    assert(std::fclose(file) == 0);
    return value;
}

int main() {
    char directory[] = "/tmp/rmb-safe-XXXXXX";
    assert(mkdtemp(directory));
    const std::string root = std::string(directory) + "/";
    const std::string target = root + "LARGE.BIN";

    { FILE* old = std::fopen(target.c_str(), "wb"); assert(old); assert(std::fwrite("OLD", 1, 3, old) == 3); std::fclose(old); }
    {
        rmb::SafeFileWriter writer;
        assert(writer.open("LARGE.BIN", "HTTP", root.c_str()));
        std::uint8_t block[2048];
        for (std::size_t i = 0; i < sizeof(block); ++i) block[i] = static_cast<std::uint8_t>(i);
        for (int i = 0; i < 512; ++i) assert(writer.write(block, sizeof(block)));
        assert(writer.bytes() == 1024u * 1024u);
        assert(writer.commit());
    }
    assert(read_all(target.c_str()).size() == 1024u * 1024u);
    {
        rmb::SafeFileWriter writer;
        assert(writer.open("LARGE.BIN", "HTTP", root.c_str()));
        assert(writer.write(reinterpret_cast<const std::uint8_t*>("BROKEN"), 6));
        writer.abort();
    }
    assert(read_all(target.c_str()).size() == 1024u * 1024u);
    assert(access((root + "HTTP.TMP").c_str(), F_OK) != 0);
    assert(access((root + "HTTP.BAK").c_str(), F_OK) != 0);
    assert(!rmb::SafeFileWriter::valid_root_name("../BAD"));
    assert(!rmb::SafeFileWriter::valid_root_name("DIR/BAD"));
    std::remove(target.c_str());
    rmdir(directory);
    std::cout << "safe streamed replace and interruption tests passed\n";
}
