// SPDX-License-Identifier: GPL-2.0-or-later
// rsh: Ruby with command lines. `rsh file [args]` runs a script (also through `#!/usr/bin/rsh`),
// `rsh` alone on a terminal is interactive. Every file (`.rsh` or `.rb`) has the shell layer; a
// first line `# rsh: ruby` turns it off (plain Ruby). See rsh(1) and docs/rsh.md.
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "edit.hpp"
#include "exec.hpp"
#include "rt.hpp"

#include <mruby.h>
#include <mruby/compile.h>
#include <mruby/error.h>
#include <mruby/irep.h>
#include <mruby/proc.h>
#include <mruby/dump.h>
#include <mruby/variable.h>

using namespace rsh;

extern "C" const uint8_t rsh_prelude_more[];
extern "C" const uint8_t rsh_prelude_std[];

namespace {

// ---- signals, trap and at_exit (0.1.4, audit H-2) ----------------------------------------------
// A handler only notes the signal; the Ruby side acts on it at its next safe point (src/prelude.rb,
// Rsh.__signals): a trap block runs, or TERM, HUP and INT with no trap raise SignalException or
// Interrupt so ensure clauses and at_exit blocks run, and rsh then ends by that signal. Code that
// reaches no safe point (a `while true` with no command in it) is ended by the signal itself
// after 2 seconds, or at once on a second one, as before.
namespace sig {
constexpr int N = 65;
volatile sig_atomic_t pend[N];
volatile sig_atomic_t mode[N];  // 0 rsh's own, 1 ignored, 2 a Ruby block
volatile sig_atomic_t any = 0;
volatile sig_atomic_t watch = 0;     // the signal the 2-second watchdog will finish
volatile sig_atomic_t soft_int = 0;  // interactive: Ctrl-C interrupts the Ruby code, not the shell
volatile sig_atomic_t first_at = 0;  // when the pending one came (seconds, monotonic)
int died = 0;                        // the signal that ended the script (rsh ends by it too)

void handle(int s);
void finish(int s) {
    signal(s, SIG_DFL);
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, s);
    sigprocmask(SIG_UNBLOCK, &m, nullptr);
    raise(s);
}
bool interactive_session = false;
void on_signal(int s) {
    if (s <= 0 || s >= N) return;
    if (s == SIGALRM && watch) {
        finish(watch);
        return;
    }
    bool soft = mode[s] == 2 || (s == SIGINT && soft_int);
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    // Another one a second or more later while the first is still not taken: now. (One sent to the
    // process and to its group, as timeout(1) does, arrives twice at once and counts once.)
    if (!soft && pend[s] && now.tv_sec - first_at >= 1) {
        finish(s);
        return;
    }
    // Waiting for a command: it gets TERM or HUP too, or the shell would sit until the 2-second
    // watchdog (the command is not in the shell's process group when it has job control).
    if ((s == SIGTERM || s == SIGHUP) && mode[s] == 0) forward_to_foreground(s);
    // INT to rsh alone while a command runs (kill -INT, timeout -s INT): the command gets it too
    // when the script has no INT trap, as Ctrl-C to the group would (0.9.1, audit H1)
    if (s == SIGINT && mode[s] == 0 && !interactive_session) forward_to_foreground(s);
    if (!pend[s]) first_at = sig_atomic_t(now.tv_sec);
    pend[s] = 1;
    any = 1;
    if (!soft && !watch) {
        watch = s;
        handle(SIGALRM);  // installed now, not at start-up (it is async-signal-safe)
        alarm(2);
    }
}
void handle(int s) {
    struct sigaction a {};
    a.sa_handler = on_signal;
    sigemptyset(&a.sa_mask);
    a.sa_flags = SA_RESTART;
    sigaction(s, &a, nullptr);
}
void set_disposition(int s, void (*h)(int)) {
    struct sigaction a {};
    a.sa_handler = h;
    sigemptyset(&a.sa_mask);
    sigaction(s, &a, nullptr);
}
// A script: TERM, HUP and INT end it through Ruby. One its parent ignores stays ignored (nohup,
// a background job of a shell without job control), as sh does.
void install_script() {
    // One system call each (start-up pays for every one): set ours, and put the old ignore back.
    for (int s : {SIGTERM, SIGHUP, SIGINT}) {
        struct sigaction a {}, old {};
        a.sa_handler = on_signal;
        sigemptyset(&a.sa_mask);
        a.sa_flags = SA_RESTART;
        sigaction(s, &a, &old);
        if (old.sa_handler == SIG_IGN) {
            mode[s] = 1;
            if (s == SIGINT) shell().int_mode = 1;
            sigaction(s, &old, nullptr);
        }
    }
}
// Interactive: while a line runs, and back to the prompt's own settings after it.
void eval_begin() {
    soft_int = 1;
    for (int s : {SIGTERM, SIGHUP, SIGINT})
        if (mode[s] != 1) handle(s);
    handle(SIGALRM);
}
void eval_end() {
    for (int s : {SIGTERM, SIGHUP})
        if (mode[s] == 0) set_disposition(s, SIG_DFL);
    if (mode[SIGINT] != 2) set_disposition(SIGINT, SIG_IGN);
    pend[SIGINT] = 0;
}

mrb_value m_sigp(mrb_state*, mrb_value) { return mrb_bool_value(any != 0); }
mrb_value m_sig_take(mrb_state*, mrb_value) {
    any = 0;
    int got = 0;
    for (int s = 1; s < N; ++s)
        if (pend[s]) {
            if (got) {
                any = 1;
                break;
            }
            got = s;
            pend[s] = 0;
            if (watch == s) {
                alarm(0);
                watch = 0;
            }
        }
    return mrb_fixnum_value(got);
}
mrb_value m_trap(mrb_state* mrb, mrb_value) {
    mrb_int n, m;
    mrb_get_args(mrb, "ii", &n, &m);
    if (n <= 0 || n >= N || n == SIGKILL || n == SIGSTOP) return mrb_nil_value();
    int s = int(n);
    mode[s] = sig_atomic_t(m);
    if (s == SIGINT) shell().int_mode = int(m);
    bool own = s == SIGTERM || s == SIGHUP || s == SIGINT;
    if (m == 1) set_disposition(s, SIG_IGN);
    else if (m == 2) handle(s);
    else if (own && !interactive_session) handle(s);
    else if (s == SIGINT) set_disposition(s, SIG_IGN);  // the prompt ignores it
    else set_disposition(s, SIG_DFL);
    if (s == SIGALRM && m != 1) handle(SIGALRM);  // the watchdog needs it
    pend[s] = 0;
    return mrb_nil_value();
}
// The exception for a signal with no trap: Interrupt or SignalException, marked as an exit with
// status 128+N (no message; rsh then ends by the signal) except Ctrl-C at the interactive prompt.
mrb_value m_signal_exc(mrb_state* mrb, mrb_value) {
    mrb_int n;
    mrb_get_args(mrb, "i", &n);
    bool intr = n == SIGINT;
    mrb_value arg = intr ? mrb_str_new_lit(mrb, "") : mrb_fixnum_value(n);
    mrb_value exc = mrb_obj_new(mrb, mrb_class_get(mrb, intr ? "Interrupt" : "SignalException"), 1, &arg);
    if (!(intr && soft_int)) {
        mrb_exc_ptr(exc)->flags |= MRB_EXC_EXIT;
        mrb_iv_set(mrb, exc, mrb_intern_lit(mrb, "status"), mrb_fixnum_value(128 + n));
        died = int(n);
    }
    return exc;
}
mrb_value m_home_self(mrb_state* mrb, mrb_value) {
    std::string h = home_of_uid(geteuid());
    return h.empty() ? mrb_nil_value() : mrb_str_new(mrb, h.data(), h.size());
}
mrb_value m_ppid(mrb_state*, mrb_value) { return mrb_fixnum_value(getppid()); }
// Rsh.__died(n): the script ends by signal n (a signal inside at_exit, audit M1).
mrb_value m_died(mrb_state* mrb, mrb_value) {
    mrb_int n;
    mrb_get_args(mrb, "i", &n);
    if (n > 0 && n < N) died = int(n);
    return mrb_nil_value();
}
// SystemExit#status and #success? (the built-in interpreter keeps the status but has no method).
mrb_value m_exit_status(mrb_state* mrb, mrb_value self) {
    mrb_value v = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "status"));
    return mrb_integer_p(v) ? v : mrb_fixnum_value(0);
}
mrb_value m_exit_success(mrb_state* mrb, mrb_value self) {
    mrb_value v = m_exit_status(mrb, self);
    return mrb_bool_value(mrb_integer(v) == 0);
}
}  // namespace sig

// src/prelude_more.rb, the first time Signal, trap, at_exit, Dir.home or Process.kill is used.
bool more_loaded = false;
mrb_value m_more(mrb_state* mrb, mrb_value) {
    if (more_loaded) return mrb_nil_value();
    more_loaded = true;
    // At the top level, as src/prelude.rb is (mrb_load_irep would take the caller's class as the
    // place for definitions: `class Dir` and `module Kernel` would nest in the caller).
    mrb_irep* irep = mrb_read_irep(mrb, rsh_prelude_more);
    if (!irep) mrb_raise(mrb, E_SCRIPT_ERROR, "rsh: src/prelude_more.rb did not load");
    RProc* proc = mrb_proc_new(mrb, irep);
    mrb_irep_decref(mrb, irep);
    MRB_PROC_SET_TARGET_CLASS(proc, mrb->object_class);
    mrb_top_run(mrb, proc, mrb_top_self(mrb), 0);
    if (mrb->exc) {
        mrb_value e = mrb_obj_value(mrb->exc);
        mrb->exc = nullptr;
        mrb_exc_raise(mrb, e);
    }
    return mrb_nil_value();
}

// src/prelude_std.rb (0.9.0), the first time Sh, FileUtils, Find, Shellwords or Dir.mktmpdir is
// used: at the top level, as prelude_more.
bool std_loaded = false;
mrb_value m_std(mrb_state* mrb, mrb_value) {
    if (std_loaded) return mrb_false_value();
    std_loaded = true;
    mrb_irep* irep = mrb_read_irep(mrb, rsh_prelude_std);
    if (!irep) mrb_raise(mrb, E_SCRIPT_ERROR, "rsh: src/prelude_std.rb did not load");
    RProc* proc = mrb_proc_new(mrb, irep);
    mrb_irep_decref(mrb, irep);
    MRB_PROC_SET_TARGET_CLASS(proc, mrb->object_class);
    mrb_top_run(mrb, proc, mrb_top_self(mrb), 0);
    if (mrb->exc) {
        mrb_value e = mrb_obj_value(mrb->exc);
        mrb->exc = nullptr;
        mrb_exc_raise(mrb, e);
    }
    return mrb_true_value();
}

// What the program adds to the interpreter rt_open made (the prelude has stand-ins for these).
void program_methods(mrb_state* mrb) {
    RClass* r = mrb_module_get(mrb, "Rsh");
    mrb_define_module_function(mrb, r, "__more", m_more, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, r, "__std", m_std, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, r, "__sigp", sig::m_sigp, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, r, "__sig_take", sig::m_sig_take, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, r, "__trap", sig::m_trap, MRB_ARGS_REQ(2));
    mrb_define_module_function(mrb, r, "__signal_exc", sig::m_signal_exc, MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, r, "__home_self", sig::m_home_self, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, r, "__ppid", sig::m_ppid, MRB_ARGS_NONE());
    mrb_define_module_function(mrb, r, "__died", sig::m_died, MRB_ARGS_REQ(1));
    RClass* se = mrb_class_get(mrb, "SystemExit");
    mrb_define_method(mrb, se, "status", sig::m_exit_status, MRB_ARGS_NONE());
    mrb_define_method(mrb, se, "success?", sig::m_exit_success, MRB_ARGS_NONE());
}

// --explain and -n agree (0.1.4, audit R2-18): the Ruby text is compiled too; its errors are
// printed as -n prints them. Returns 2 when there are any.
int compile_errors(const std::string& ruby, const std::string& file) {
    mrb_state* mrb = mrb_open();
    if (!mrb) return 0;
    mrb_ccontext* c = mrb_ccontext_new(mrb);
    mrb_ccontext_filename(mrb, c, file.c_str());
    c->capture_errors = TRUE;
    c->no_exec = TRUE;
    int bad = 0;
    mrb_parser_state* p = mrb_parse_nstring(mrb, ruby.data(), ruby.size(), c);
    if (!p) bad = 2;
    else {
        for (size_t e = 0; e < p->nerr && e < 10; ++e) {
            std::fprintf(stderr, "%s:%d:%d: %s\n", file.c_str(), p->error_buffer[e].lineno, p->error_buffer[e].column,
                         p->error_buffer[e].message ? p->error_buffer[e].message : "syntax error");
            bad = 2;
        }
        mrb_parser_free(p);
    }
    mrb_ccontext_free(mrb, c);
    mrb_close(mrb);
    return bad;
}

// The at_exit blocks (and a trap("EXIT") block); returns the status the program ends with.
int run_at_exit(mrb_state* mrb, int status) {
    if (!more_loaded) return status;  // no at_exit or trap was ever called
    mrb_value v = mrb_funcall(mrb, mrb_obj_value(mrb_module_get(mrb, "Rsh")), "__run_at_exit", 1, mrb_fixnum_value(status));
    if (mrb->exc) {
        mrb->exc = nullptr;
        return status;
    }
    return mrb_integer_p(v) ? int(mrb_integer(v)) : status;
}

const char* USAGE =
    "usage: rsh [options] [file [arguments]]\n"
    "  -c TEXT     run TEXT (-e is the same)\n"
    "  -s          read the script from standard input (arguments follow, after --)\n"
    "  --name FILE with -s: the file name to use in messages and in $0\n"
    "  -n          check the script: read and compile it, run nothing\n"
    "  --lint      -n, plus warnings (unknown commands, risky forms)\n"
    "  --explain   show the mode and how each line is read: C command line, R Ruby, X a capture inside Ruby\n"
    "  --ruby      show the Ruby text rsh runs\n"
    "  -i          interactive (-l, a login window, is the same)\n"
    "  -v          version (--version is the same)\n"
    "  -h          this text (--help is the same)\n"
    "  --          the end of the options: the next word is the file\n";

// ---- interactive: colours and completion ---------------------------------------------------------

const char* GREEN = "\x1b[32m";
const char* RED = "\x1b[31m";
const char* YELLOW = "\x1b[33m";
const char* BLUE = "\x1b[34m";
const char* MAGENTA = "\x1b[35m";
const char* CYAN = "\x1b[36m";
const char* GREY = "\x1b[90m";
const char* RESET = "\x1b[0m";

Known* repl_known = nullptr;

std::string paint_ruby(const std::string& s, size_t from) {
    std::string o = s.substr(0, from);
    size_t i = from;
    while (i < s.size()) {
        char c = s[i];
        if (c == '#' && !(i + 1 < s.size() && s[i + 1] == '{')) {
            o += GREY + s.substr(i) + RESET;
            break;
        }
        if (c == '"' || c == '\'' || c == '`') {
            size_t e = i + 1;
            while (e < s.size() && s[e] != c) e += s[e] == '\\' ? 2 : 1;
            e = std::min(e + 1, s.size());
            o += (c == '`' ? MAGENTA : YELLOW) + s.substr(i, e - i) + RESET;
            i = e;
            continue;
        }
        if (std::isdigit((unsigned char)c)) {
            size_t e = i;
            while (e < s.size() && (std::isalnum((unsigned char)s[e]) || s[e] == '_' || s[e] == '.')) ++e;
            o += CYAN + s.substr(i, e - i) + RESET;
            i = e;
            continue;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t e = i;
            while (e < s.size() && (std::isalnum((unsigned char)s[e]) || s[e] == '_' || s[e] == '?' || s[e] == '!')) ++e;
            std::string w = s.substr(i, e - i);
            if (is_keyword(w)) o += BLUE + w + RESET;
            else if (std::isupper((unsigned char)w[0])) o += CYAN + w + RESET;
            else o += w;
            i = e;
            continue;
        }
        if (c == ':' && i + 1 < s.size() && std::isalpha((unsigned char)s[i + 1])) {
            size_t e = i + 1;
            while (e < s.size() && (std::isalnum((unsigned char)s[e]) || s[e] == '_')) ++e;
            o += CYAN + s.substr(i, e - i) + RESET;
            i = e;
            continue;
        }
        o += c;
        ++i;
    }
    return o;
}

std::string paint_cmd(const std::string& s, size_t from) {
    std::string o = s.substr(0, from);
    size_t i = from;
    bool first = true;
    while (i < s.size()) {
        char c = s[i];
        if (c == ' ' || c == '\t') {
            o += c;
            ++i;
            continue;
        }
        if (c == '|' || c == '&' || c == ';' || c == '>' || c == '<') {
            size_t e = i;
            while (e < s.size() && std::strchr("|&;<>", s[e])) ++e;
            o += BLUE + s.substr(i, e - i) + RESET;
            if (s.compare(i, 2, "||") == 0 || s.compare(i, 2, "&&") == 0 || s[i] == '|' || s[i] == ';') first = true;
            i = e;
            continue;
        }
        if (c == '#' && !(i + 1 < s.size() && s[i + 1] == '{')) {
            o += GREY + s.substr(i) + RESET;
            break;
        }
        size_t e = i;
        std::string w;
        std::string painted;
        while (e < s.size() && s[e] != ' ' && s[e] != '\t' && !std::strchr("|&;<>", s[e])) {
            if (s[e] == '\'' || s[e] == '"') {
                size_t q = e + 1;
                while (q < s.size() && s[q] != s[e]) q += s[q] == '\\' ? 2 : 1;
                q = std::min(q + 1, s.size());
                painted += YELLOW + s.substr(e, q - e) + RESET;
                w += s.substr(e, q - e);
                e = q;
            } else if (s[e] == '#' && e + 1 < s.size() && s[e + 1] == '{') {
                size_t q = s.find('}', e);
                q = q == std::string::npos ? s.size() : q + 1;
                painted += MAGENTA + s.substr(e, q - e) + RESET;
                w += s.substr(e, q - e);
                e = q;
            } else if (s[e] == '$') {
                size_t q = e + 1;
                while (q < s.size() && (std::isalnum((unsigned char)s[q]) || s[q] == '_' || s[q] == '{' || s[q] == '}' || s[q] == '?')) ++q;
                painted += CYAN + s.substr(e, q - e) + RESET;
                w += s.substr(e, q - e);
                e = q;
            } else {
                painted += s[e];
                w += s[e];
                ++e;
            }
        }
        if (first) {
            std::string name = w;
            if (name.compare(0, 2, "$ ") == 0) name = name.substr(2);
            bool found = is_builtin(name) || !find_program(name).empty();
            o += std::string(found ? GREEN : RED) + w + RESET;
            first = false;
        } else if (!w.empty() && w[0] == '-') o += CYAN + w + RESET;
        else o += painted;
        i = e;
    }
    return o;
}

std::string paint(const std::string& line) {
    if (!repl_known) return line;
    size_t before = table().size();
    Known k = *repl_known;
    Result r = transpile(line, "(rsh)", k);
    table().resize(before);
    size_t lead = 0;
    while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
    bool command = false;
    for (auto& m : r.marks)
        if (m.kind == 'C' && m.line == 1) command = true;
    if (command) {
        // Ruby words before the command (if, unless...) stay Ruby-coloured.
        size_t start = lead;
        for (const char* kw : {"if ", "unless ", "while ", "until ", "elsif ", "$ "}) {
            size_t n = std::strlen(kw);
            if (line.compare(start, n, kw) == 0) {
                std::string pre = line.substr(0, start + n);
                return paint_ruby(pre, 0) + paint_cmd(line.substr(start + n), 0);
            }
        }
        return paint_cmd(line, lead);
    }
    return paint_ruby(line, 0);
}

std::vector<std::string> complete(const std::string& line, size_t start) {
    std::string typed = line.substr(start);
    std::string word = unquote_word(typed);
    std::set<std::string> c;
    // A command word: at the start, or after | ; & && || ( (0.1.4, R2-15).
    size_t b = start;
    while (b > 0 && (line[b - 1] == ' ' || line[b - 1] == '\t')) --b;
    bool first = b == 0 || std::strchr("|;&(", line[b - 1]);
    if (b >= 2 && line.compare(b - 2, 2, "$ ") == 0) first = true;
    // `$HO` offers the environment variables that begin so (QA-RSH-52).
    if (word.size() > 1 && word[0] == '$' && word.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_", 1) == std::string::npos) {
        for (char** e = environ; e && *e; ++e) {
            const char* eq = std::strchr(*e, '=');
            std::string name = eq ? std::string(*e, size_t(eq - *e)) : std::string(*e);
            if (!name.compare(0, word.size() - 1, word, 1, std::string::npos)) c.insert("$" + name);
        }
        if (!c.empty()) return std::vector<std::string>(c.begin(), c.end());
    }
    if (first && word.find('/') == std::string::npos) {
        for (const char* b : {"cd", "pushd", "popd", "dirs", "export", "unset", "exec", "umask", "jobs", "fg", "bg", "wait",
                              "source", "echo", "type", "pwd", "history"})
            if (!std::strncmp(b, word.c_str(), word.size())) c.insert(b);
        for (const char* const* w = RUBY_WORDS; *w; ++w)
            if (!std::strncmp(*w, word.c_str(), word.size()) && std::islower((unsigned char)**w)) c.insert(*w);
        if (repl_known)
            for (auto& m : repl_known->methods)
                if (!m.compare(0, word.size(), word)) c.insert(m);
        const char* p = getenv("PATH");
        std::string path = p ? p : "/usr/bin";
        size_t i = 0;
        while (i <= path.size() && !word.empty()) {
            size_t e = path.find(':', i);
            if (e == std::string::npos) e = path.size();
            std::string dir = path.substr(i, e - i);
            if (DIR* d = opendir(dir.c_str())) {
                while (dirent* x = readdir(d))
                    if (!std::strncmp(x->d_name, word.c_str(), word.size()) && x->d_name[0] != '.') c.insert(x->d_name);
                closedir(d);
            }
            i = e + 1;
        }
        if (!c.empty()) return std::vector<std::string>(c.begin(), c.end());
    }
    if (repl_known && !first)
        for (auto& l : repl_known->locals)
            if (!word.empty() && !l.compare(0, word.size(), word)) c.insert(l);
    std::string dir = ".", base = word, shown_dir;
    size_t slash = word.rfind('/');
    if (slash != std::string::npos) {
        shown_dir = word.substr(0, slash + 1);
        dir = slash == 0 ? "/" : word.substr(0, slash);
        base = word.substr(slash + 1);
        if (dir[0] == '~') dir = home_of("") + dir.substr(1);
    }
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* x = readdir(d)) {
            if (std::strncmp(x->d_name, base.c_str(), base.size())) continue;
            if (x->d_name[0] == '.' && (base.empty() || base[0] != '.')) continue;
            if (!std::strcmp(x->d_name, ".") || !std::strcmp(x->d_name, "..")) continue;
            std::string full = (dir == "/" ? "/" : dir + "/") + x->d_name;
            struct stat st;
            bool isdir = stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
            // Inserted as one word of the line: `sp ace` becomes sp\ ace (M-7, R2-16).
            c.insert(quote_word(shown_dir, true) + quote_word(x->d_name) + (isdir ? "/" : ""));
        }
        closedir(d);
    }
    return std::vector<std::string>(c.begin(), c.end());
}

std::string short_cwd(const std::string& h) {
    std::string c = current_dir();
    if (c.empty()) return "?";
    if (!h.empty() && h != "/" && c.compare(0, h.size(), h) == 0 && (c.size() == h.size() || c[h.size()] == '/'))
        c = "~" + c.substr(h.size());
    return printable(c);  // a folder name with terminal escapes is shown, not obeyed (audit M3)
}

// Root, or a set-user-id start: the person's environment ($HOME, $XDG_*) may point into another
// person's files (sudo -E, su without -), so it is not used for init.rsh and history (audit H3).
// The text of a script as the reader takes it (0.1.4): a UTF-8 byte-order mark at the start is
// dropped (editors on Windows write one; it hid `# rsh: ruby`, audit R2-6) and every CR LF is LF,
// as Ruby reads its source (a here-document end word and a `\` at a line end work, audit R2-5).
std::string source_text(std::string s) {
    if (s.compare(0, 3, "\xEF\xBB\xBF") == 0) s.erase(0, 3);
    if (s.find("\r\n") == std::string::npos) return s;
    std::string o;
    o.reserve(s.size());
    for (size_t k = 0; k < s.size(); ++k)
        if (!(s[k] == '\r' && k + 1 < s.size() && s[k + 1] == '\n')) o += s[k];
    return o;
}

// `# rsh: WORD` as the mode comment (the first line, or the one after #!) with a WORD that is not
// ruby or shell: a typo there silently gave the shell layer to a file meant as plain Ruby (M-12).
// Returns the line number of such a comment, else 0; `word` gets the word.
int unknown_mode(const std::string& src, std::string& word) {
    size_t b = 0;
    for (int n = 0; n < 2 && b < src.size(); ++n) {
        size_t e = src.find('\n', b);
        if (e == std::string::npos) e = src.size();
        std::string line = src.substr(b, e - b);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (n == 0 && line.compare(0, 2, "#!") == 0) {
            b = e + 1;
            continue;
        }
        if (line.compare(0, 1, "#") != 0) return 0;
        size_t k = 1;
        while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
        if (line.compare(k, 4, "rsh:") != 0) return 0;
        k += 4;
        while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
        word = line.substr(k);
        return word == "ruby" || word == "shell" ? 0 : n + 1;
    }
    return 0;
}

bool privileged() { return geteuid() == 0 || geteuid() != getuid() || getegid() != getgid(); }

int interactive() {
    setup_interactive();
    mrb_state* mrb = rt_open("rsh", {});
    if (!mrb) return 70;
    program_methods(mrb);
    sig::interactive_session = true;
    const bool strict = privileged();
    std::string home = strict ? home_of_uid(geteuid()) : home_of("");
    if (home.empty()) home = "/";
    auto from_env = [&](const char* name) -> std::string {
        const char* v = strict ? nullptr : getenv(name);
        return v && *v ? v : "";
    };
    std::string cfg = from_env("XDG_CONFIG_HOME");
    std::string init = (cfg.empty() ? home + "/.config" : cfg) + "/rsh/init.rsh";
    {
        std::string why;
        int fd = open_own(init, O_RDONLY, strict, why);
        if (fd >= 0) {  // read from the file that was checked, not the name again
            std::string src;
            char b[65536];
            ssize_t n;
            while ((n = read(fd, b, sizeof b)) > 0) src.append(b, size_t(n));
            close(fd);
            sig::eval_begin();
            rt_run(mrb, src, init, true);
            sig::eval_end();
        } else if (!why.empty())
            std::fprintf(stderr, "rsh: not running %s: %s\n", printable(init).c_str(), why.c_str());
    }
    std::string xs = from_env("XDG_STATE_HOME");
    std::string state = xs.empty() ? home + "/.local/state" : xs;
    if (xs.empty()) mkdir((home + "/.local").c_str(), 0700);
    mkdir(state.c_str(), 0700);
    mkdir((state + "/rsh").c_str(), 0700);
    Editor ed;
    ed.strict = strict;
    {
        std::string why;
        if (own_dir(state + "/rsh", strict, why)) ed.history_file = state + "/rsh/history";
        else if (!why.empty()) std::fprintf(stderr, "rsh: history is not kept in a file this time: %s: %s\n", printable(state + "/rsh").c_str(), why.c_str());
    }
    ed.load();
    // `history` lists the entries of earlier sessions too, then this one's (audit M-9).
    for (auto& e : ed.history) rt_add_history(e);
    ed.paint = paint;
    ed.complete = complete;
    Known known;
    repl_known = &known;
    std::string buf;
    int status = 0;
    bool warned_jobs = false;  // exit or Ctrl-D with jobs running: said once, the second one leaves
    auto jobs_left = [&] {
        report_jobs();
        if (warned_jobs || running_jobs() == 0) return false;
        warned_jobs = true;
        std::fprintf(stderr, "rsh: there are jobs still running (jobs lists them); exit again to leave them\n");
        return true;
    };
    for (;;) {
        report_jobs();
        std::string prompt;
        if (!buf.empty()) prompt = std::string(GREY) + "  ... " + RESET;
        else if (!rt_prompt(mrb, prompt))
            prompt = std::string("\x1b[1;34m") + short_cwd(home) + RESET + (status ? std::string(RED) + " " + std::to_string(status) + RESET : "") +
                     " \x1b[1;35m>\x1b[0m ";
        std::string line;
        if (!ed.read(prompt, line)) {
            if (!buf.empty()) {
                buf.clear();
                continue;
            }
            if (isatty(0) && jobs_left()) continue;
            break;
        }
        buf += line + "\n";
        bool exited, incomplete;
        sig::eval_begin();
        int st = rt_eval(mrb, buf, known, exited, incomplete);
        sig::eval_end();
        if (incomplete) continue;
        std::string entry = buf.substr(0, buf.size() - 1);
        size_t had = ed.history.size();
        ed.add(entry);
        if (ed.history.size() > had) rt_add_history(entry);
        buf.clear();
        status = st;
        if (exited && !sig::died && isatty(0) && jobs_left()) continue;
        if (exited) {
            st = run_at_exit(mrb, st);
            int died = sig::died;
            rt_close(mrb);
            if (died && st == 128 + died) sig::finish(died);
            return st;
        }
    }
    status = run_at_exit(mrb, status);
    rt_close(mrb);
    return status;
}

}  // namespace

int main(int argc, char** argv) {
    std::string code, file, name;
    bool have_code = false, from_stdin = false, check = false, lint = false, explain = false, show_ruby = false, inter = false;
    int i = 1;
    for (; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--") {
            ++i;
            break;
        }
        if (a.empty() || a[0] != '-' || a == "-") break;
        if ((a == "-c" || a == "-e") && i + 1 < argc && have_code) {  // only the last ran (audit 2 L7)
            std::fprintf(stderr, "rsh: -c (or -e) given twice: one text per run\n%s", USAGE);
            return 2;
        }
        if ((a == "-c" || a == "-e") && i + 1 < argc) code = argv[++i], have_code = true;
        else if (a == "-s") from_stdin = true;
        else if (a == "--name" && i + 1 < argc) name = argv[++i];
        else if (a == "-n") check = true;
        else if (a == "--lint") check = lint = true;
        else if (a == "--explain") explain = true;
        else if (a == "--ruby") show_ruby = true;
        else if (a == "-i" || a == "-l") inter = true;
        else if (a == "-v" || a == "--version") {
            std::printf("rsh %s\n", RSH_VERSION);
            return 0;
        } else if (a == "-h" || a == "--help") {
            std::fputs(USAGE, stdout);
            return 0;
        } else if (a == "-c" || a == "-e" || a == "--name") {  // the last word: its value is missing (L-8)
            std::fprintf(stderr, "rsh: %s needs %s\n%s", a.c_str(), a == "--name" ? "a FILE" : "TEXT", USAGE);
            return 2;
        } else {
            std::fprintf(stderr, "rsh: unknown option %s\n%s", a.c_str(), USAGE);
            return 2;
        }
    }
    std::vector<std::string> args;
    std::string src;
    if (have_code) {
        file = "-c";
        src = code;
    } else if (from_stdin) {
        file = name.empty() ? "-" : name;
        bool ok;
        src = read_file("-", ok);
    } else if (i < argc) {
        file = argv[i++];
        struct stat sb;
        if (stat(file.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) {  // it read as an empty script (M-11)
            std::fprintf(stderr, "rsh: %s: Is a directory\n", file.c_str());
            return 126;
        }
        bool ok;
        src = read_file(file, ok);
        if (!ok) {
            std::fprintf(stderr, "rsh: %s: %s\n", file.c_str(), std::strerror(errno));
            return 127;
        }
    } else if (inter || isatty(0)) {
        return interactive();
    } else {
        file = "-";
        bool ok;
        src = read_file("-", ok);
    }
    for (; i < argc; ++i) args.push_back(argv[i]);
    src = source_text(std::move(src));
    {
        std::string word;
        if (int at = unknown_mode(src, word)) {
            std::fprintf(stderr, "%s%s:%d: unknown mode `%s` (the mode comment is # rsh: ruby or # rsh: shell)\n",
                         check || explain ? "" : "rsh: ", file.c_str(), at, word.c_str());
            return 2;
        }
    }

    // The shell layer is on unless the file starts with `# rsh: ruby` (after a `#!` line, if any).
    const bool shell = wants_shell_layer(file, src);

    if (explain || show_ruby) {
        Known k;
        Result r;
        if (shell) {
            size_t slash = file.rfind('/');
            (void)slash;
            collect_defs(src, script_dir(file), k.methods);
            r = transpile(src, file, k);
        } else r.ruby = src;
        for (auto& d : r.diags)
            std::fprintf(stderr, "%s:%d: %s%s\n", file.c_str(), d.line, d.warning ? "warning: " : "", d.msg.c_str());
        if (show_ruby) std::fputs(r.ruby.c_str(), stdout);
        else {
            std::printf("mode: %s\n", shell ? "shell" : "ruby (the shell layer is off: `# rsh: ruby` on the first line)");
            // The names the rule reads as Ruby methods (def, define_method :x, attr_*, rsh_methods, ...),
            // and the lines where no bare word is a command.
            if (shell && !k.methods.empty()) {
                std::string m;
                for (auto& x : k.methods) m += " " + x;
                std::printf("methods:%s\n", m.c_str());
            }
            for (auto& sp : r.ruby_spans) {
                if (sp.second == sp.first) std::printf("ruby only: line %d (a ruby block)\n", sp.first);
                else if (sp.second) std::printf("ruby only: lines %d-%d (a ruby block)\n", sp.first, sp.second);
                else std::printf("ruby only: line %d to the end (ruby!)\n", sp.first);
            }
            std::vector<char> kind;
            size_t n = 1;
            for (char c : src) n += c == '\n';
            kind.assign(n + 1, 'R');
            for (auto& m : r.marks)
                if (m.line > 0 && size_t(m.line) <= n && kind[size_t(m.line)] != 'C') kind[size_t(m.line)] = m.kind;
            size_t ln = 1, b = 0;
            while (b <= src.size()) {
                size_t e = src.find('\n', b);
                if (e == std::string::npos) e = src.size();
                std::string text = src.substr(b, e - b);
                bool blank = text.find_first_not_of(" \t") == std::string::npos;
                std::printf("%4zu %c  %s\n", ln, blank ? ' ' : kind[ln], text.c_str());
                ++ln;
                b = e + 1;
                if (e == src.size()) break;
            }
        }
        if (!r.ok()) return 2;
        return explain ? compile_errors(r.ruby, file) : 0;
    }
    mrb_state* mrb = rt_open(file, args);
    if (!mrb) return 70;
    program_methods(mrb);
    if (!check) sig::install_script();
    int rc = check ? rt_check(mrb, src, file, lint, shell) : rt_run(mrb, src, file, shell);
    if (!check) rc = run_at_exit(mrb, rc);
    std::fflush(stdout);
    int died = sig::died;
    rt_close(mrb);
    if (died && rc == 128 + died) sig::finish(died);  // as sh: the parent sees the signal
    return rc;
}
