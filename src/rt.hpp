// SPDX-License-Identifier: GPL-2.0-or-later
// rsh: the embedded interpreter and what rsh adds to it (Rsh module, Regexp engine, JSON, ENV).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "transpile.hpp"

struct mrb_state;

namespace rsh {

mrb_state* rt_open(const std::string& script, const std::vector<std::string>& args);
void rt_close(mrb_state* mrb);

// Reads, transpiles and runs a file or a text. Returns the exit status the process should end
// with (0 when it ran to its end). `known` may carry names between interactive inputs.
int rt_run(mrb_state* mrb, const std::string& src, const std::string& file, bool rsh_syntax);

// Interactive input: run one complete text in the shared context; prints errors, never exits
// unless the code calls exit (then `exited` is set and the status returned).
int rt_eval(mrb_state* mrb, const std::string& src, Known& known, bool& exited, bool& incomplete);

// -n / --lint: transpile and compile only. Prints problems; returns 0 when clean.
// With rsh_syntax false the text is only parsed as Ruby (a file marked `# rsh: ruby`).
int rt_check(mrb_state* mrb, const std::string& src, const std::string& file, bool lint, bool rsh_syntax = true);

// True when `file` (with this text) is read with the shell layer: every file, whatever its name,
// unless `# rsh: ruby` is its first line (or the one after a `#!` line).
bool wants_shell_layer(const std::string& file, const std::string& src);

// Calls the person's `prompt` method when init.rsh defines one.
bool rt_prompt(mrb_state* mrb, std::string& out);

std::string read_file(const std::string& path, bool& ok);
// The folder require_relative and a written-out `source` path are read from: that of the script's
// real path (links resolved); "." for -c, standard input and (rsh eval).
std::string script_dir(const std::string& file);

// A Regexp subject for the engine: 2 ASCII only, 1 valid UTF-8, -1 not valid UTF-8 (the engine's
// rules; a subject called valid is matched without the engine's own check).
int utf8_kind(const char* s, size_t n);

// Where the engine finds invalid UTF-8 (the offset of each bad character); writes up to cap.
size_t utf8_bad(const char* s, size_t n, size_t* out, size_t cap);

#ifdef PCRE2_H_IDEMPOTENT_GUARD  // files that include <pcre2.h> first
// One match as with PCRE2_MATCH_INVALID_UTF, from the valid pieces (src/rt.cpp).
int frag_match(const pcre2_code* code, const char* p, size_t len, size_t start, const size_t* bad, size_t nbad,
               uint32_t lookbehind, pcre2_match_data* md, pcre2_match_context* cx);
#endif
void rt_add_history(const std::string& line);

}  // namespace rsh
