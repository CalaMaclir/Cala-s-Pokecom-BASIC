#include "command_history.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    rmb::CommandHistory history;
    char line[rmb::CommandHistory::kEntrySize] = {};

    history.append("");
    assert(history.size() == 0);
    history.append("PRINT 1");
    history.append("PRINT 2");
    history.append("RUN");
    history.append("RUN");
    assert(history.size() == 3);

    assert(history.previous("PRI", line, sizeof(line)));
    assert(std::strcmp(line, "RUN") == 0);
    assert(history.previous(line, line, sizeof(line)));
    assert(std::strcmp(line, "PRINT 2") == 0);
    assert(history.previous(line, line, sizeof(line)));
    assert(std::strcmp(line, "PRINT 1") == 0);
    assert(!history.previous(line, line, sizeof(line)));

    assert(history.next(line, sizeof(line)));
    assert(std::strcmp(line, "PRINT 2") == 0);
    assert(history.next(line, sizeof(line)));
    assert(std::strcmp(line, "RUN") == 0);
    assert(history.next(line, sizeof(line)));
    assert(std::strcmp(line, "PRI") == 0);
    assert(!history.next(line, sizeof(line)));

    for (std::size_t i = 0; i < rmb::CommandHistory::kCapacity + 5; ++i) {
        char command[32] = {};
        std::snprintf(command, sizeof(command), "CMD%02u", static_cast<unsigned>(i));
        history.append(command);
    }
    assert(history.size() == rmb::CommandHistory::kCapacity);
    assert(std::strcmp(history.at(0), "CMD05") == 0);
    assert(std::strcmp(history.at(history.size() - 1), "CMD28") == 0);

    std::puts("Command history: PASS");
}
