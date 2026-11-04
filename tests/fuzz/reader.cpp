// SPDX-License-Identifier: GPL-2.0-or-later
// rsh: a fuzz target for the reader (0.9.0 stability contract, docs/STABILITY.md). Any text goes
// through what `rsh -n` does with a file: the definition pre-pass, the reader (statement starts,
// the command-line rule, the command-line grammar, here-documents) and the interpreter's parser on
// the Ruby the reader wrote; the first line also goes through the one-command-line parser
// (`system`, `Rsh.run`). Nothing runs. A crash, a sanitizer report or a hang is a finding.
// Build and run: `make fuzz` (clang with -fsanitize=fuzzer); FUZZ_SECONDS sets the length.
#include <mruby.h>
#include <mruby/compile.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "transpile.hpp"

using namespace rsh;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static mrb_state* mrb = mrb_open();
    if (size > 64 * 1024) return 0;  // a script, not a data file: keeps every run fast
    std::string src(reinterpret_cast<const char*>(data), size);
    size_t before = table().size();
    Known k;
    collect_defs(src, "/nonexistent-rsh-fuzz", k.methods);  // the require_relative and source read-ahead parse, find nothing
    Result r = transpile(src, "fuzz.rsh", k);
    if (r.ok()) {
        int ai = mrb_gc_arena_save(mrb);
        mrb_ccontext* c = mrb_ccontext_new(mrb);
        c->capture_errors = TRUE;
        c->no_exec = TRUE;
        mrb_parser_state* p = mrb_parse_nstring(mrb, r.ruby.data(), r.ruby.size(), c);
        if (p) mrb_parser_free(p);
        mrb_ccontext_free(mrb, c);
        mrb_gc_arena_restore(mrb, ai);
    }
    std::string first = src.substr(0, src.find('\n'));
    std::vector<std::string> values;
    std::string err;
    parse_command(first, "fuzz.rsh", 1, values, err);
    table().resize(before);  // the table only grows in a real run; here every input starts afresh
    return 0;
}
