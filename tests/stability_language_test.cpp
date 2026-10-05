#include "language93_test_support.hpp"
#include "soak_support.hpp"

int main() {
    const std::string classic =
        "DIM A(3)\nDATA 2,4,6\nFOR I=1 TO 3\nREAD A(I)\nNEXT I\n"
        "RESTORE\nREAD B\nS=0\nFOR I=1 TO 3\nGOSUB 160\nNEXT I\n"
        "PRINT S;B;TRIM$(\" OK \" );REPLACE$(\"aaaa\",\"aa\",\"b\")\n"
        "GOTO 150\nPRINT \"BAD\"\nEND\nS=S+A(I)\nRETURN\n";
    const std::string structured =
        "S=0\nFOR I=1 TO 3\nS=S+Add(I)\nNEXT I\n"
        "IF S=12 THEN\nPRINT \"OK\"\nELSEIF S=0 THEN\nPRINT \"BAD\"\n"
        "ELSE\nPRINT \"BAD\"\nEND IF\nK=0\nWHILE K<2\nDO\n"
        "K=K+1\nEXIT DO\nLOOP\nWEND\nSELECT CASE K\n"
        "CASE 1\nPRINT \"BAD\"\nCASE 2\nPRINT S;K\nCASE ELSE\n"
        "PRINT \"BAD\"\nEND SELECT\nDATA 4,5\nREAD A,B\nRESTORE\nREAD C\n"
        "PRINT A;B;C\nFUNCTION Add(N)\nRETURN N*2\nEND FUNCTION\n";
    const auto baseline = cpb_soak::resources();
    const auto psram_bytes = rmb::psram::used_bytes();
    cpb_soak::record("classic-structured", 0, baseline);
    for (unsigned i = 0; i < cpb_soak::iterations(); ++i) {
        // Six execution paths per optimizer setting, including cache restores.
        language93_case(classic, "122OKbb\r\n", true);
        language93_case(structured, "OK\r\n122\r\n454\r\n");
        assert(rmb::psram::used_bytes() == psram_bytes);
        if ((i + 1) % 50 == 0 || i + 1 == cpb_soak::iterations())
            cpb_soak::record("classic-structured", i + 1, baseline);
    }
    std::puts("Classic / Structured repeated compile / VM / cache / arrays / DATA / strings / nested control: PASS");
}
