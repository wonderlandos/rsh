// SPDX-License-Identifier: GPL-2.0-or-later
// rsh: the line editor for interactive use (raw terminal, history, completion, colours).
#pragma once
#include <string>
#include <vector>

namespace rsh {

struct Editor {
    std::vector<std::string> history;
    std::string history_file;
    bool strict = false;  // root or setuid: group-writable files are refused too (audit H3)
    // Colours one input line for display (same visible width as the text).
    std::string (*paint)(const std::string& line) = nullptr;
    // Candidates for the word that starts at `start` of `line`.
    std::vector<std::string> (*complete)(const std::string& line, size_t start) = nullptr;

    // Reads history_file; when it is not safe to use (a link, another owner, not a plain file) it
    // says so on standard error and clears history_file: the session keeps history in memory only.
    void load();
    // Adds an entry; lines that start with a space or look like they hold a secret are not kept.
    void add(const std::string& entry);
    // false at end of input (Ctrl-D on an empty line).
    bool read(const std::string& prompt, std::string& out);
};

size_t visible_width(const std::string& s);
// Text from outside (file names, the current folder, history) made safe to write to a terminal:
// control bytes as ^X (^? for 0x7f), C1 controls and bytes that are not UTF-8 as \xNN (audit M3).
std::string printable(const std::string& s);
// A line history must not keep: it starts with a space, or sets or holds what looks like a secret
// (NAME=... where NAME has TOKEN, SECRET, PASSWORD, PASSWD, PASSPHRASE, API_KEY, PRIVATE_KEY,
// ACCESS_KEY or CREDENTIAL; well-known token prefixes; a private key block; Authorization headers).
bool keep_out_of_history(const std::string& line);
// Opens `path` (never through a symbolic link, close-on-exec) when it is one's own file: a plain
// file owned by the effective user, not writable by others (with `strict`, not by its group either).
// -1 otherwise; `why` then says what is wrong ("" when the file does not exist).
int open_own(const std::string& path, int flags, bool strict, std::string& why);
// The same test for a folder (lstat: a link is refused); `why` is "" when it does not exist.
bool own_dir(const std::string& path, bool strict, std::string& why);

}  // namespace rsh
