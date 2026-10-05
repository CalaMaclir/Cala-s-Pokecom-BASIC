#pragma once
#include "program_store.hpp"
#include "system_information.hpp"
#include "vm.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rmb {
enum class ErrorPhase : std::uint8_t { Compile, Runtime };
// One bounded boot-session record. No borrowed source pointers survive RUN.
// No invented error code or column: those are not exposed by the interpreter.
struct ErrorContext {
    bool valid = false, direct = false, source_available = false, source_truncated = false;
    ErrorPhase phase = ErrorPhase::Runtime;
    ProgramSourceMode mode = ProgramSourceMode::ClassicNumbered;
    std::uint64_t revision = 0;
    std::uint32_t uptime_ms = 0;
    std::int32_t location = 0, pc = -1;
    char program[80] = {}, message[96] = {}, source[192] = {};
    char function[kSymbolNameLength] = {};
    std::uint16_t call_depth = 0;
    VmResult::CallSite trace[VmResult::kTraceCapacity] = {};
    std::uint8_t trace_count = 0;
    bool trace_truncated = false;
    const char* type_name() const { return phase == ErrorPhase::Compile ? "Compile Error" : "Runtime Error"; }
    const char* mode_name() const { return direct ? "Direct" : mode == ProgramSourceMode::Structured ? "Structured" : "Classic"; }
    void capture(ErrorPhase kind, const char* cause, const ProgramStore& store,
                 const char* filename, std::int32_t at, std::uint32_t now,
                 const char* direct_text = nullptr) {
        *this = {};
        valid = true; phase = kind; direct = direct_text != nullptr;
        mode = store.source_mode(); revision = store.revision(); uptime_ms = now;
        location = direct ? 0 : at;
        std::snprintf(program, sizeof(program), "%s", direct ? "(direct)" : filename && *filename ? filename : "(unsaved)");
        std::snprintf(message, sizeof(message), "%s", cause ? cause : "ERROR");
        if (direct) { copy_source(direct_text); return; }
        if (at <= 0 || !store.ready()) return;
        // Fetch only on failure. Metadata avoids reading every SD source row.
        for (std::size_t i=0; i<store.size(); ++i) {
            std::int32_t number = 0; std::size_t length = 0;
            if (!store.read_line_metadata(i, number, length)) return;
            if (number != at) continue;
            const char* text = nullptr;
            if (store.read_line_text(i, number, text, length)) copy_source(text);
            return;
        }
    }
    void copy_source(const char* text) {
        if (!text) return;
        source_available = true;
        source_truncated = std::strlen(text) >= sizeof(source);
        std::snprintf(source, sizeof(source), "%s", text);
        for (char* p=source; *p; ++p)
            if (static_cast<unsigned char>(*p)<32 && *p!='\t') *p='?';
    }
    bool can_edit(const ProgramStore& store, const char* filename) const {
        if (!valid || direct || !source_available || location<=0 || !store.ready() ||
            revision!=store.revision() || mode!=store.source_mode() ||
            std::strcmp(program, filename ? filename : "")) return false;
        std::int32_t number=0; std::size_t length=0;
        if (mode == ProgramSourceMode::Structured)
            return static_cast<std::size_t>(location)<=store.size();
        for (std::size_t i=0; i<store.size(); ++i)
            if (store.read_line_metadata(i,number,length) && number==location) return true;
        return false;
    }
    void append_report(SystemInformation& info) const {
        info.section("Runtime / Last Error");
        if (!valid) { info.add("Last Error", "None"); return; }
        info.add("Last Error Type", "%s", type_name());
        info.add("Last Error Message", "%s", message);
        info.add("Error Program", "%s", program);
        info.add("Source Mode", "%s", mode_name());
        if (!direct && location>0) info.add(mode==ProgramSourceMode::Structured?"Source Row":"BASIC Line", "%ld", static_cast<long>(location));
        if (*function) info.add("Function", "%s", function);
        if (call_depth) info.add("Call depth", "%u%s", call_depth, trace_truncated?" (trace limited)":"");
        for (std::size_t i=0; i<trace_count; ++i)
            info.add("Call (inner first)", "%s from row %ld", trace[i].name, static_cast<long>(trace[i].caller_row));
        if (pc>=0) info.add("Error IL PC", "%ld", static_cast<long>(pc));
        info.add("Error Uptime", "%lu ms", static_cast<unsigned long>(uptime_ms));
        // Source literals may contain credentials. Never copy program text to
        // the shareable Serial snapshot; it remains available on the error LCD.
    }
};
static_assert(sizeof(ErrorContext)<768, "Last Error must remain bounded");
} // namespace rmb
