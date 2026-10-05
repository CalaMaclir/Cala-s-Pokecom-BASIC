#include "language93_test_support.hpp"
#include <filesystem>
#include <fstream>

static std::string sample(const char* name, bool numbered = false) {
    // CTest sets WORKING_DIRECTORY to CPB_ROOT. __FILE__ may be rewritten
    // relative to the build directory by ccache, so it is not a data path.
    const auto path = std::filesystem::path("examples/v094") / name;
    std::ifstream file(path);
    assert(file);
    std::string text, row;
    while (std::getline(file, row)) {
        if (numbered) {
            const auto space = row.find(' ');
            assert(space != std::string::npos);
            row.erase(0, space + 1);
        }
        text += row + '\n';
    }
    return text;
}

int main() {
    using rmb::platform::host_datetime;
    using rmb::platform::host_datetime_available;
    const std::string strings = "[CPB]\r\nbXXa\r\nA   B\r\n8!!!\r\n";
    language93_case(sample("stage8-strings-classic.bas", true), strings, true);
    language93_case(sample("stage8-strings-structured.bas"), strings);
    host_datetime_available = true; host_datetime = {2026,10,4,19,32,44};
    language93_case(sample("stage8-datetime.bas"), "2026-10-04\r\n19:32:44\r\n");
    for (bool classic : {false, true}) {
        language93_case("PRINT \"[\"+TRIM$(\"  CPB  \")+\"]\"\n"
                        "PRINT \"[\"+LTRIM$(\"  A  \")+\"]\"\n"
                        "PRINT \"[\"+RTRIM$(\"  A  \")+\"]\"\n"
                        "PRINT REPLACE$(\"banana\",\"an\",\"X\")\n"
                        "PRINT REPLACE$(\"aaaa\",\"aa\",\"b\")\n"
                        "PRINT REPLACE$(\"abc\",\"\",\"X\")\n"
                        "PRINT REPLACE$(\"a-b-a\",\"a\",\"\")\n"
                        "PRINT LEN(SPACE$(191));LEN(SPACE$(0))\n"
                        "PRINT \"A\"+SPACE$(2.9)+\"B\"\n"
                        "PRINT INSTR(\"CPB\",\"P\");STRING$(3,\"!\")\n",
                        "[CPB]\r\n[A  ]\r\n[  A]\r\nbXXa\r\nbb\r\nabc\r\n-b-\r\n1910\r\nA  B\r\n2!!!\r\n",classic);
        language93_case("PRINT \"[\"+TRIM$(CHR$(9)+\"A\"+CHR$(13)+CHR$(10))+\"]\"\n"
                        "PRINT LEN(TRIM$(\"   \"));LEN(TRIM$(\"\"))\n",
                        "[A]\r\n00\r\n",classic);
        for (const char* expression : {"TRIM$(1)","LTRIM$(1)","RTRIM$(1)",
                "REPLACE$(1,\"a\",\"b\")","SPACE$(\"x\")"})
            language93_case(std::string("PRINT ")+expression+"\n","",classic,"TYPE MISMATCH");
        for (const char* expression : {"TRIM$()","TRIM$(\"a\",\"b\")", "REPLACE$(\"a\",\"b\")",
                "SPACE$()","DATE$(1)","TIME$(1)"})
            language93_case(std::string("PRINT ")+expression+"\n","",classic,"ARGUMENT COUNT");
        for (const char* expression : {"SPACE$(-1)","SPACE$(192)","SPACE$(1E30)"})
            language93_case(std::string("PRINT ")+expression+"\n","",classic,"BAD COUNT");
        language93_case("PRINT REPLACE$(STRING$(191,\"a\"),\"a\",\"aa\")\n",
                        "",classic,"STRING TOO LONG");
        // Parentheses keep existing DATE$/TIME$ scalar programs working.
        language93_case("DATE$=\"legacy date\":TIME$=\"legacy time\"\nPRINT DATE$;TIME$\n",
                        "legacy datelegacy time\r\n",classic);
        host_datetime_available=true;host_datetime={2026,10,4,19,32,44};
        language93_case("PRINT DATE$();\" \";TIME$()\n","2026-10-04 19:32:44\r\n",classic);
        host_datetime={2028,2,29,0,0,0};
        language93_case("PRINT DATE$();\" \";TIME$()\n","2028-02-29 00:00:00\r\n",classic);
        host_datetime_available=false;
        language93_case("PRINT DATE$()\n","",classic,"RTC NOT AVAILABLE");
        language93_case("PRINT TIME$()\n","",classic,"RTC NOT AVAILABLE");
    }
    host_datetime_available=true;host_datetime={2026,10,4,19,32,44};
    language93_case("PRINT Clean$(\"  CPB  \")\nFUNCTION Clean$(S$)\nRETURN REPLACE$(TRIM$(S$),\"P\",\"p\")+SPACE$(1)+DATE$()\nEND FUNCTION\n",
                    "CpB 2026-10-04\r\n");
    host_datetime_available=false;
    std::puts("Daily string/date functions: Classic / Structured / FUNCTION / cache / error PASS");
}
