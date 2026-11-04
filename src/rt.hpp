// SPDX-License-Identifier: GPL-2.0-or-later
// rsh: the embedded interpreter and what rsh adds to it (Rsh module, Regexp engine, JSON, ENV).
#pragma once
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
// With rsh_syntax false the text is only parsed as Ruby (a plain .rb file).
int rt_check(mrb_state* mrb, const std::string& src, const std::string& file, bool lint, bool rsh_syntax = true);

// True when `file` (with this text) is read with the shell layer: every file but a `.rb` file
// that does not carry `# rsh: shell` as its first line (or the one after a `#!` line).
bool wants_shell_layer(const std::string& file, const std::string& src);

// Calls the person's `prompt` method when init.rsh defines one.
bool rt_prompt(mrb_state* mrb, std::string& out);

std::string read_file(const std::string& path, bool& ok);
void rt_add_history(const std::string& line);

}  // namespace rsh
