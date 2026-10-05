#include "language93_test_support.hpp"
#include <fstream>
#include <iterator>

static std::string read_all(const char* path) {
    std::ifstream file(path);
    assert(file.good());
    return std::string(std::istreambuf_iterator<char>(file), {});
}

static void classic_file(const char* path, const char* expected) {
    Language93Session fixture;
    assert(fixture.source.new_program(rmb::ProgramSourceMode::ClassicNumbered));
    std::ifstream file(path);
    assert(file.good());
    std::string line;
    while (std::getline(file, line)) {
        char* end = nullptr;
        const auto number = std::strtol(line.c_str(), &end, 10);
        while (*end == ' ') ++end;
        assert(number > 0 && fixture.source.set_line(number, end));
    }
    const auto result = fixture.compiler.compile(fixture.source, *fixture.code);
    if (!result.ok) std::cerr << path << ": " << result.message << "\n";
    assert(result.ok);
    fixture.check_runs(expected);
}

int main() {
    language93_case(
        read_all("examples/v093/manual/select-menu-structured.bas"),
        "SENSOR RANGE\r\n"
    );
    language93_case(
        read_all("examples/v093/manual/function-structured.bas"),
        "15\r\n120\r\n[CPB]\r\n"
    );
    language93_case(
        read_all("examples/v093/manual/exit-cleanup-structured.bas"),
        "1\r\n2\r\n3\r\n"
    );
    classic_file(
        "examples/v093/manual/data-restore-classic.bas",
        "15 READY\r\nRESTORE=7\r\n"
    );
    classic_file(
        "examples/v093/manual/math-intops-classic.bas",
        "3\r\n1\r\n135\r\n3\r\n16\r\n-5\r\n6\r\n"
    );
    std::puts("v0.93 manual executable examples: PASS (5 files, Classic and Structured)");
}
