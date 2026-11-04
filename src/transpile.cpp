// SPDX-License-Identifier: GPL-2.0-or-later
// rsh: the reader. A small Ruby lexer finds statement starts; at each one the first word decides
// between Ruby and a command line (docs/rsh.md, "How rsh reads a line"). The rule looks
// only at the text, never at PATH, so a file means the same thing on every computer.
#include "transpile.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace rsh {

std::vector<Line>& table() {
    static std::vector<Line> t;
    return t;
}

const char* const RUBY_WORDS[] = {
    "puts", "print", "p", "pp", "printf", "sprintf", "format", "require", "require_relative", "load",
    "raise", "fail", "loop", "exit", "abort", "at_exit", "catch", "throw", "lambda", "proc",
    "rand", "srand", "gets", "warn", "binding", "caller", "Integer", "Float", "String", "Array",
    "Hash", "attr_reader", "attr_writer", "attr_accessor", "private", "public", "protected",
    "module_function", "include", "extend", "define_method", "system", "trap", "freeze",
    "__method__",
    // metaprogramming: these take their arguments without parentheses as often as with them
    "send", "public_send", "__send__", "eval", "instance_eval", "instance_exec", "class_eval", "class_exec",
    "module_eval", "module_exec", "define_singleton_method", "instance_variable_get", "instance_variable_set",
    "remove_method", "undef_method", "alias_method", "prepend", "using", "refine", "private_constant",
    "public_constant", "private_class_method", "public_class_method", "remove_const", "const_get", "const_set",
    "singleton_class", "method", "rsh_methods",
    // conversions every object has, called without a receiver inside methods
    "to_a", "to_s", "to_h", "to_i", "to_f", "to_sym", "to_proc", "inspect", "dup", "clone", "instance_variables",
    // 0.1.3: the rest of the built-in interpreter's Kernel functions (audit M1). `open "x"` opens a
    // file, it never runs a program called open. `sleep` stays a command (`sleep 5 &`).
    "open", "putc", "readline", "readlines", "__dir__", "__callee__", "global_variables", "local_variables",
    "usleep", "tap", "itself", "object_id", "enum_for", "to_enum", "yield_self", "methods", "singleton_methods",
    "public_methods", "private_methods", "protected_methods", "remove_instance_variable", "singleton_method",
    // From 0.1.3 the list is frozen (docs/STABILITY.md, tests/contract.sh): it only grows, in a major version.
    nullptr};

static const char* const KEYWORDS[] = {
    "alias", "and", "begin", "BEGIN", "break", "case", "class", "def", "defined?", "do", "else",
    "elsif", "end", "END", "ensure", "false", "for", "if", "in", "module", "next", "nil", "not",
    "or", "redo", "rescue", "retry", "return", "self", "super", "then", "true", "undef", "unless",
    "until", "when", "while", "yield", "__FILE__", "__LINE__", "__ENCODING__", nullptr};

static bool in_list(const char* const* l, const std::string& w) {
    for (; *l; ++l)
        if (w == *l) return true;
    return false;
}
bool is_keyword(const std::string& w) { return in_list(KEYWORDS, w); }
bool is_ruby_word(const std::string& w) { return in_list(RUBY_WORDS, w); }

bool Result::ok() const {
    for (auto& d : diags)
        if (!d.warning) return false;
    return true;
}

namespace {

bool ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }
bool ident_char(char c) { return std::isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }
bool blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

int count_nl(const std::string& s) {
    int n = 0;
    for (char c : s) n += c == '\n';
    return n;
}

// From s[i] (just inside a '{'), the index of the matching '}', or npos. Knows strings, nested
// braces and comments; enough for #{...} and Ruby blocks inside command lines.
size_t match_brace(const std::string& s, size_t i, size_t lim) {
    int depth = 0;
    while (i < lim) {
        char c = s[i];
        if (c == '\\') {
            i += 2;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            char q = c;
            ++i;
            while (i < lim && s[i] != q) {
                if (s[i] == '\\') ++i;
                else if (q != '\'' && s[i] == '#' && i + 1 < lim && s[i + 1] == '{') {
                    size_t e = match_brace(s, i + 2, lim);
                    if (e == std::string::npos) return e;
                    i = e;
                }
                ++i;
            }
            ++i;
            continue;
        }
        if (c == '#' && !(i + 1 < lim && s[i + 1] == '{')) {
            while (i < lim && s[i] != '\n') ++i;
            continue;
        }
        if (c == '{') ++depth;
        else if (c == '}') {
            if (depth == 0) return i;
            --depth;
        }
        ++i;
    }
    return std::string::npos;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string first_line(const std::string& s) { return trim(s.substr(0, s.find('\n'))); }

std::string fragment(const std::string& src, const std::string& file, int line, Known& known, Result& res);

// ---- command lines -------------------------------------------------------------------------

struct CmdParser {
    const std::string& s;
    size_t i, lim;
    char close;  // ')' or '}': an unquoted close at a word start ends the line
    std::string file;
    int line0;
    Known& known;
    Result& res;
    std::vector<std::string> vals;
    std::string err;
    int nl = 0;
    bool capture = false;
    bool do_block = false;
    // `cmd || exit 1`, `cmd && return`: after && or || a Ruby control word ends the command line and
    // the rest of the statement is Ruby (0.1.4, audit R2-7). `tail` is where it starts, npos if none.
    size_t tail = std::string::npos;
    bool tail_or = false, tail_colon = false;
    bool after_andor = false;
    // After `then`, `else` or `do` on the same line: a last word `end` (or `else`, `elsif`) closes
    // the Ruby construct, it is not an argument (`if x then echo a end`, audit M-8).
    bool inline_end = false;
    struct Pending {
        size_t item, cmd, redir;
        std::string delim;
        bool squiggly, literal;
    };
    std::vector<Pending> here;
    size_t n_items = 0, n_cmds = 0;  // where the command being read will sit (here-doc fix-up)
    void push_item(Line& L, Item& it) {
        L.items.push_back(std::move(it));
        it = Item();
        ++n_items;
        n_cmds = 0;
    }

    CmdParser(const std::string& s_, size_t i_, size_t lim_, char close_, const std::string& f, int l, Known& k,
              Result& r)
        : s(s_), i(i_), lim(lim_), close(close_), file(f), line0(l), known(k), res(r) {}

    bool fail(const std::string& m) {
        if (err.empty()) err = m;
        return false;
    }
    int value(const std::string& ruby_src) {
        vals.push_back(fragment(ruby_src, file, line0 + nl, known, res));
        return int(vals.size()) - 1;
    }
    void skip_blank() {
        while (i < lim) {
            if (blank(s[i])) ++i;
            else if (s[i] == '\\' && i + 1 < lim && s[i + 1] == '\n') {
                i += 2;
                ++nl;
            } else break;
        }
    }
    void skip_newlines() {  // after | && || a line may go on
        for (;;) {
            skip_blank();
            if (i < lim && s[i] == '\n') {
                ++i;
                ++nl;
            } else if (i < lim && s[i] == '#' && !(i + 1 < lim && s[i + 1] == '{')) {
                while (i < lim && s[i] != '\n') ++i;
            } else break;
        }
    }
    bool line_end() const {
        if (i >= lim) return true;
        char c = s[i];
        return c == '\n' || c == ';' || (close && c == close);
    }
    static void add(Word& w, Part::Kind k, const std::string& t) {
        if (!w.parts.empty() && w.parts.back().k == k && (k == Part::Lit || k == Part::Pat)) w.parts.back().s += t;
        else {
            Part p;
            p.k = k;
            p.s = t;
            w.parts.push_back(p);
        }
    }
    static void add_val(Word& w, Part::Kind k, int idx, const std::string& name = "") {
        Part p;
        p.k = k;
        p.idx = idx;
        p.s = name;
        w.parts.push_back(p);
    }
    bool interp(Word& w) {  // at "#{"
        size_t e = match_brace(s, i + 2, lim);
        if (e == std::string::npos) return fail("#{ without a closing }");
        std::string expr = s.substr(i + 2, e - i - 2);
        nl += count_nl(expr);
        add_val(w, Part::Val, value("(" + expr + ")"));
        i = e + 1;
        return true;
    }
    bool dollar(Word& w) {  // at '$'
        char c = i + 1 < lim ? s[i + 1] : 0;
        if (c == '?') add_val(w, Part::Status, -1), i += 2;
        else if (c == '*') add_val(w, Part::Args, -1), i += 2;
        else if (c == '!') add_val(w, Part::BgPid, -1), i += 2;
        else if (c == '$') add_val(w, Part::Val, value("(Process.pid)")), i += 2;  // audit L-3
        else if (c == '(') return fail("$(...) is not rsh: capture with #{`command`} or a Ruby variable");
        else if (c == '{') {
            size_t e = s.find('}', i + 2);
            if (e == std::string::npos || e >= lim) return fail("${ without a closing }");
            if (e == i + 2) return fail("${} needs a variable name: ${HOME}");
            std::string name = s.substr(i + 2, e - i - 2);
            // ${NAME:-x}, ${#NAME}, ${NAME%.c} ... are sh; they read as a variable with that odd
            // name and failed only when the line ran (0.9.0): say it when the file is read.
            bool plain = ident_start(name[0]);
            for (char ch : name) plain = plain && ident_char(ch);
            if (!plain) {
                size_t op = 0;
                while (op < name.size() && ident_char(name[op])) ++op;
                std::string var = op ? name.substr(0, op) : "NAME";
                return fail("${" + name + "} is sh: ${...} takes a variable name only; write #{ENV.fetch(\"" + var +
                            "\", \"default\")} for a default, or #{...} with Ruby for the rest");
            }
            add_val(w, Part::Env, -1, name);
            i = e + 1;
        } else if (std::isdigit((unsigned char)c) || c == '@' || c == '#')
            return fail(std::string("$") + c + " is sh: write #{ARGV[0]}, #{ARGV.size} or $*");
        else if (ident_start(c)) {
            size_t e = i + 1;
            while (e < lim && ident_char(s[e])) ++e;
            add_val(w, Part::Env, -1, s.substr(i + 1, e - i - 1));
            i = e;
        } else {
            add(w, Part::Lit, "$");
            ++i;
        }
        return true;
    }
    bool dquote(Word& w) {  // at '"'
        ++i;
        add(w, Part::Lit, "");
        while (i < lim && s[i] != '"') {
            char c = s[i];
            if (c == '\\' && i + 1 < lim) {
                char d = s[i + 1];
                if (d == '\n') ++nl;
                else if (std::strchr("\"\\$#`", d)) add(w, Part::Lit, std::string(1, d));
                else add(w, Part::Lit, std::string("\\") + d);
                i += 2;
            } else if (c == '#' && i + 1 < lim && s[i + 1] == '{') {
                if (!interp(w)) return false;
            } else if (c == '$') {
                if (!dollar(w)) return false;
            } else {
                if (c == '\n') ++nl;
                add(w, Part::Lit, std::string(1, c));
                ++i;
            }
        }
        if (i >= lim) return fail("a \" string is not closed");
        ++i;
        return true;
    }
    static bool word_stop(char c) { return blank(c) || c == '\n' || std::strchr(";|&<>()", c); }
    bool word(Word& w) {
        while (i < lim && !word_stop(s[i])) {
            char c = s[i];
            if (c == '#' && i + 1 < lim && s[i + 1] == '{') {
                if (!interp(w)) return false;
            } else if (c == '\'') {
                size_t e = s.find('\'', i + 1);
                if (e == std::string::npos || e >= lim) return fail("a ' string is not closed");
                std::string t = s.substr(i + 1, e - i - 1);
                nl += count_nl(t);
                add(w, Part::Lit, t);
                i = e + 1;
            } else if (c == '"') {
                if (!dquote(w)) return false;
            } else if (c == '\\' && i + 1 < lim) {
                if (s[i + 1] == '\n') ++nl;
                else add(w, Part::Lit, std::string(1, s[i + 1]));
                i += 2;
            } else if (c == '$') {
                if (!dollar(w)) return false;
            } else if (c == '`') {
                return fail("a capture inside a command line: put it in a variable first");
            } else {
                add(w, Part::Pat, std::string(1, c));
                ++i;
            }
        }
        return true;
    }
    static bool plain(const Word& w, const char* t) {
        return w.parts.size() == 1 && w.parts[0].k == Part::Pat && w.parts[0].s == t;
    }
    bool finish_cmd(Item& it, Cmd& c) {
        if (c.words.empty() && c.block < 0) {
            if (!c.env.empty()) return fail("NAME=value needs a command after it (for the shell's own variables use export or ENV)");
            return fail("a command is missing");
        }
        it.pipeline.push_back(std::move(c));
        c = Cmd();
        ++n_cmds;
        return true;
    }
    bool redirect(Cmd& c, int fd) {
        Redir r;
        if (s[i] == '&') {  // &> &>>
            i += 2;
            r.k = Redir::Both;
            if (i < lim && s[i] == '>') r.k = Redir::BothAppend, ++i;
        } else if (s[i] == '>') {
            r.fd = fd < 0 ? 1 : fd;
            if (i + 1 < lim && s[i + 1] == '>') r.k = Redir::Append, i += 2;
            else if (i + 1 < lim && s[i + 1] == '&') {
                i += 2;
                if (i >= lim || !std::isdigit((unsigned char)s[i])) return fail(">& needs a file descriptor number");
                r.k = Redir::Dup;
                r.dupfd = s[i++] - '0';
                c.redirs.push_back(r);
                return true;
            } else r.k = Redir::Out, ++i;
        } else {  // '<'
            r.fd = fd < 0 ? 0 : fd;
            if (s.compare(i, 3, "<<<") == 0) {
                i += 3;
                skip_blank();
                r.k = Redir::Here;
                if (!word(r.target) || r.target.parts.empty()) return fail("<<< needs a word");
                add(r.target, Part::Lit, "\n");
                c.redirs.push_back(r);
                return true;
            }
            if (s.compare(i, 2, "<<") == 0) {
                i += 2;
                Pending p;
                p.squiggly = i < lim && (s[i] == '~' || s[i] == '-');
                if (p.squiggly) ++i;
                p.literal = i < lim && (s[i] == '\'' || s[i] == '"');
                char q = p.literal ? s[i++] : 0;
                size_t b = i;
                while (i < lim && (q ? s[i] != q : ident_char(s[i]))) ++i;
                p.delim = s.substr(b, i - b);
                if (q) ++i;
                p.literal = q == '\'';
                if (p.delim.empty()) return fail("<< needs an end word (<<EOF)");
                if (capture) return fail("here-documents do not go in a capture");
                r.k = Redir::Here;
                c.redirs.push_back(r);
                p.redir = c.redirs.size() - 1;
                p.item = n_items;
                p.cmd = n_cmds;
                here.push_back(p);
                return true;
            }
            r.k = Redir::In;
            ++i;
        }
        skip_blank();
        if (!word(r.target) || r.target.parts.empty()) return fail("a redirect needs a file name");
        c.redirs.push_back(r);
        return true;
    }
    bool env_prefix(Cmd& c, Word& w) {
        if (!c.words.empty() || w.parts.empty() || w.parts[0].k != Part::Pat) return false;
        const std::string& t = w.parts[0].s;
        size_t eq = t.find('=');
        if (eq == 0 || eq == std::string::npos || !ident_start(t[0])) return false;
        for (size_t k = 0; k < eq; ++k)
            if (!ident_char(t[k])) return false;
        Word v;
        if (eq + 1 < t.size()) add(v, Part::Pat, t.substr(eq + 1));
        for (size_t k = 1; k < w.parts.size(); ++k) v.parts.push_back(w.parts[k]);
        c.env.emplace_back(t.substr(0, eq), v);
        return true;
    }

    bool parse(Line& L) {
        Item it;
        Cmd c;
        bool ruby_after = false;
        bool after_pipe = false;  // `echo a |` with nothing after it (audit M-2)
        for (;;) {
            skip_blank();
            if (line_end()) break;
            if (s[i] == '#' && !(i + 1 < lim && s[i + 1] == '{')) {
            } else after_pipe = after_pipe && s[i] == '|';
            char ch = s[i];
            char nx = i + 1 < lim ? s[i + 1] : 0;
            if (ch == '#' && nx != '{') {
                while (i < lim && s[i] != '\n') ++i;
                break;
            }
            if (ch == '}' && close == '}') break;
            if (ch == '|') {
                ruby_after = false;
                if (nx == '|') {
                    if (!finish_cmd(it, c)) return false;
                    it.op = Op::Or;
                    push_item(L, it);
                    i += 2;
                    skip_newlines();
                    if (ruby_tail(true)) break;
                    after_andor = true;
                    continue;
                }
                if (nx == '&') c.pipe_err = true, ++i;
                if (!finish_cmd(it, c)) return false;
                ++i;
                skip_newlines();
                after_pipe = true;
                if (i < lim && s[i] == '{') {
                    size_t e = match_brace(s, i + 1, lim);
                    if (e == std::string::npos) return fail("a Ruby stage { |line| ... } is not closed");
                    std::string body = s.substr(i, e + 1 - i);
                    nl += count_nl(body);
                    Cmd rb;
                    rb.block = value("proc " + body);
                    i = e + 1;
                    c = std::move(rb);
                    ruby_after = true;
                }
                continue;
            }
            if (ch == '&' && nx == '&') {
                ruby_after = false;
                if (!finish_cmd(it, c)) return false;
                it.op = Op::And;
                push_item(L, it);
                i += 2;
                skip_newlines();
                if (ruby_tail(false)) break;
                after_andor = true;
                continue;
            }
            if (ch == '&' && nx != '>') {
                if (!finish_cmd(it, c)) return false;
                it.op = Op::Bg;
                push_item(L, it);
                ++i;
                continue;
            }
            if (ruby_after) return fail("after a Ruby stage only | may follow");
            if (ch == '>' || ch == '<' || (ch == '&' && nx == '>')) {
                if (!redirect(c, -1)) return false;
                continue;
            }
            if (std::isdigit((unsigned char)ch) && (nx == '>' || nx == '<') && c.words.size() + c.env.size() > 0) {
                ++i;
                if (!redirect(c, ch - '0')) return false;
                continue;
            }
            if (ch == '(' || ch == ')') {
                if (after_andor && ch == '(' && c.words.empty())
                    return fail("( ) do not group commands in rsh: after && or || write the Ruby itself (cmd || exit 1)");
                return fail("( ) do not group commands in rsh: use Ruby (if, begin ... end)");
            }
            if (ch == '{' && after_andor && c.words.empty() && (nx == ' ' || nx == '\t'))
                return fail("{ ...; } does not group commands in rsh: after && or || write the Ruby itself (cmd || exit 1)");
            after_andor = false;
            if (ch == '!' && c.words.empty() && (nx == ' ' || nx == '\t')) {
                it.bang = true;
                ++i;
                continue;
            }
            Word w;
            size_t word_at = i;
            if (!word(w)) return false;
            if (w.parts.empty()) return fail(s[i] == '\0' ? std::string("unexpected NUL byte (\\0)") : std::string("unexpected ") + s[i]);
            if (inline_end && !c.words.empty() && (plain(w, "end") || plain(w, "else") || plain(w, "elsif"))) {
                i = word_at;
                break;
            }
            if (env_prefix(c, w)) continue;
            if (!c.words.empty()) {
                if (plain(w, "do")) {
                    skip_blank();
                    if (plain(c.words[0], "cd") && line_end() && L.items.empty()) {
                        do_block = true;
                        break;
                    }
                    return fail("`do` in a command line: only `cd dir do ... end` takes a block; quote it ('do') to pass the word");
                }
                for (const char* kw : {"if", "unless", "while", "until", "rescue", "then"})
                    if (plain(w, kw))
                        return fail(std::string("`") + kw + "` in a command line is passed as a word; quote it ('" + kw +
                                    "') or put the Ruby on its own line");
            }
            c.words.push_back(std::move(w));
        }
        if (after_pipe && c.words.empty() && c.block < 0 && c.env.empty() && c.redirs.empty())
            return fail("a command is missing after |");
        if (!c.words.empty() || c.block >= 0 || !c.env.empty() || !c.redirs.empty()) {
            if (!finish_cmd(it, c)) return false;
        }
        if (!it.pipeline.empty()) push_item(L, it);
        else if (!L.items.empty() && L.items.back().op != Op::Bg && tail == std::string::npos)
            return fail("a command is missing after && or ||");
        if (L.items.empty()) return fail("an empty command line");
        if (L.items.back().op == Op::Bg) {
        } else L.items.back().op = Op::End;
        if (!here.empty()) {
            if (tail != std::string::npos) return fail("a here-document line cannot go on in Ruby after && or ||: put the Ruby on its own line");
            return bodies(L);
        }
        return true;
    }
    // At a word after && or ||: exit, raise, abort, return, break, next, fail, throw (Ruby's control
    // words; never programs that matter) or a lone `:` (sh's no-op).
    bool ruby_tail(bool is_or) {
        if (capture || i >= lim) return false;
        while (i < lim && blank(s[i])) ++i;
        if (i >= lim) return false;
        // `cmd && $ echo x`: the escape hatch forces a command here too (0.9.1, audit H3)
        if (s[i] == '$' && i + 1 < lim && blank(s[i + 1])) {
            i += 2;
            while (i < lim && blank(s[i])) ++i;
            return false;
        }
        if (ruby_word_here()) {
            tail = i;
            tail_or = is_or;
            return true;
        }
        size_t e = i;
        if (s[i] == ':') {
            e = i + 1;
            if (e < lim && !blank(s[e]) && s[e] != '\n' && s[e] != ';' && s[e] != '#') return false;
            tail_colon = true;
        } else {
            while (e < lim && ident_char(s[e])) ++e;
            if (e < lim && s[e] == '!') ++e;
            std::string w = s.substr(i, e - i);
            bool ok = false;
            for (const char* t : {"exit", "exit!", "raise", "fail", "abort", "return", "break", "next", "throw"}) ok |= w == t;
            if (!ok) return false;
            if (e < lim && !blank(s[e]) && !std::strchr("\n;(#", s[e])) return false;
        }
        tail = tail_colon ? e : i;
        tail_or = is_or;
        return true;
    }
    // After && or ||, the word is Ruby by the same rules as at the start of a statement (S1.3 rules
    // 2-5, S1.4, S1.5; 0.9.1, the CTO's decision on audit 2 H3): a local, a method the file makes,
    // a Ruby word, a Capitalised name (not NAME=value cmd), a keyword value, or a word with `.`, `(`,
    // `[`, `::`, `,` or an assignment after it. A path is a command, as at a statement start.
    bool ruby_word_here() const {
        if (!ident_start(s[i])) return false;
        size_t j = i;
        while (j < lim && ident_char(s[j])) ++j;
        std::string w = s.substr(i, j - i);
        if (j < lim && (s[j] == '?' || s[j] == '!') && !(j + 1 < lim && s[j + 1] == '=')) ++j;
        if (std::isupper((unsigned char)w[0])) {
            // NAME=value cmd stays a command line, as at a statement start
            return !(j < lim && s[j] == '=' && j + 1 < lim && !std::strchr("=~", s[j + 1]));
        }
        for (const char* k : {"true", "false", "nil", "self", "not", "defined", "yield", "super"})
            if (w == k) return true;
        if (is_keyword(w)) return false;  // then, do, end, else ...: as before
        if (known.locals.count(w) || known.methods.count(w) || is_ruby_word(w)) return true;
        if (j < lim && std::strchr(".([,", s[j])) return true;
        if (j + 1 < lim && s[j] == ':' && s[j + 1] == ':') return true;
        // an assignment: n=1, n = 1, n += 1, n ||= 1 (not == or =~; `echo =x` stays a command)
        size_t k = j;
        while (k < lim && blank(s[k])) ++k;
        size_t a = k;
        while (a < lim && a - k < 3 && std::strchr("+-*/|&<>%", s[a])) ++a;
        if (a >= lim || s[a] != '=' || (a + 1 < lim && (s[a + 1] == '=' || s[a + 1] == '~'))) return false;
        if (a == j) return true;                          // glued: n=1
        return a + 1 >= lim || blank(s[a + 1]) || a > k;  // spaced: n = 1; an operator: n += 1, n +=1

    }
    // Here-document bodies: the lines after this one, up to each end word.
    bool bodies(Line& L) {
        if (i >= lim || s[i] != '\n') return fail("a here-document must end its line");
        ++i;
        ++nl;
        for (auto& p : here) {
            std::vector<std::string> lines;
            bool found = false;
            while (i < lim) {
                size_t e = s.find('\n', i);
                if (e == std::string::npos || e > lim) e = lim;
                std::string ln = s.substr(i, e - i);
                i = e < lim ? e + 1 : lim;
                ++nl;
                if ((p.squiggly ? trim(ln) : ln) == p.delim) {
                    found = true;
                    break;
                }
                lines.push_back(ln);
            }
            if (!found) return fail("here-document without its end word " + p.delim);
            if (p.squiggly) {  // drop the common indentation
                size_t ind = std::string::npos;
                for (auto& ln : lines) {
                    if (trim(ln).empty()) continue;
                    size_t k = 0;
                    while (k < ln.size() && blank(ln[k])) ++k;
                    if (k < ind) ind = k;
                }
                for (auto& ln : lines) ln = ln.size() >= ind && ind != std::string::npos ? ln.substr(ind) : trim(ln);
            }
            std::string text;
            for (auto& ln : lines) text += ln + "\n";
            Word w;
            if (p.literal) add(w, Part::Lit, text);
            else {
                // #{...} and $NAME work. As in sh a backslash is taken away only before $, `, \, a
                // newline (and #{ here); before anything else it stays (\n in a printf format, a
                // Windows path, a regex: 0.1.4, audit M-3).
                size_t k = 0;
                std::string lit;
                auto flush = [&] {
                    if (!lit.empty()) add(w, Part::Lit, lit);
                    lit.clear();
                };
                while (k < text.size()) {
                    char ch = text[k];
                    if (ch == '\\' && k + 1 < text.size()) {
                        char d = text[k + 1];
                        if (d == '\n') {
                        } else if (d == '$' || d == '`' || d == '\\' || (d == '#' && k + 2 < text.size() && text[k + 2] == '{')) lit += d;
                        else lit += std::string("\\") + d;
                        k += 2;
                    } else if (ch == '#' && k + 1 < text.size() && text[k + 1] == '{') {
                        size_t e = match_brace(text, k + 2, text.size());
                        if (e == std::string::npos) return fail("#{ without } in a here-document");
                        flush();
                        add_val(w, Part::Val, value("(" + text.substr(k + 2, e - k - 2) + ")"));
                        k = e + 1;
                    } else if (ch == '$' && k + 1 < text.size() && (ident_start(text[k + 1]) || text[k + 1] == '{')) {
                        flush();
                        size_t b = k + 1, e;
                        if (text[b] == '{') {
                            e = text.find('}', b);
                            if (e == std::string::npos) return fail("${ without }");
                            add_val(w, Part::Env, -1, text.substr(b + 1, e - b - 1));
                            k = e + 1;
                        } else {
                            e = b;
                            while (e < text.size() && ident_char(text[e])) ++e;
                            add_val(w, Part::Env, -1, text.substr(b, e - b));
                            k = e;
                        }
                    } else {
                        lit += ch;
                        ++k;
                    }
                }
                flush();
                if (w.parts.empty()) add(w, Part::Lit, "");
            }
            L.items[p.item].pipeline[p.cmd].redirs[p.redir].target = w;
        }
        return true;
    }
};

// ---- the Ruby side -------------------------------------------------------------------------

struct Tx {
    const std::string& s;
    size_t n, i = 0;
    std::string out;
    std::string file;
    int line;
    Known& known;
    Result& res;
    enum Prev { Start, Value, Op } prev = Start;
    bool stmt = true, cond = false, space = true, after_dot = false, lambda = false, rescue_line = false;
    std::vector<char> frames{'T'};
    struct Heredoc {
        std::string id;
        bool dash;
    };
    std::vector<Heredoc> heredocs;
    // Ruby-only regions: `ruby do ... end` (closed by the `end` that brings `kw` back below `kw`),
    // `ruby { ... }` (closed by the `}` of frame `frame`), and `ruby!` (to the end of the file).
    // `kw` counts the open keyword constructs (class def if ... do ... end).
    int kw = 0;
    bool loop_do = false;       // after while/until/for: their `do` opens nothing
    bool inline_kw = false;     // then/else/do earlier on this line (CmdParser::inline_end)
    bool pending_ruby = false;  // `ruby` at a statement start, its `do` or `{` comes next
    int pending_line = 0;
    struct Region {
        int kw;
        size_t frame;
        int line;
    };
    std::vector<Region> regions;
    bool plain_rest = false;

    Tx(const std::string& src, const std::string& f, int l, Known& k, Result& r)
        : s(src), n(src.size()), file(f), line(l), known(k), res(r) {}

    bool at(const char* t) const { return s.compare(i, std::strlen(t), t) == 0; }
    void copy(size_t e) {
        line += count_nl(s.substr(i, e - i));
        out.append(s, i, e - i);
        i = e;
    }
    void error(int ln, const std::string& m) { res.diags.push_back({ln, m, false}); }
    void warning(int ln, const std::string& m) { res.diags.push_back({ln, m, true}); }
    bool forced_now = false;  // the command line being read was written `$ cmd`

    void flush_heredocs() {
        for (auto& h : heredocs) {
            while (i < n) {
                size_t e = s.find('\n', i);
                size_t end = e == std::string::npos ? n : e + 1;
                std::string ln = s.substr(i, (e == std::string::npos ? n : e) - i);
                copy(end);
                if ((h.dash ? trim(ln) : ln) == h.id) break;
            }
        }
        heredocs.clear();
    }

    // Strings with #{...}: the code inside is read as Ruby (so captures in it are rewritten).
    void dstring(char close, char open) {
        int depth = 0;
        while (i < n) {
            char c = s[i];
            if (c == '\\' && i + 1 < n) {
                copy(i + 2);
                continue;
            }
            if (c == '#' && i + 1 < n && s[i + 1] == '{') {
                out += "#{";
                i += 2;
                frames.push_back('I');
                Prev p0 = prev;
                bool st0 = stmt;
                prev = Start;
                stmt = false;
                code(true);
                prev = p0;
                stmt = st0;
                frames.pop_back();
                if (i < n) out += '}', ++i;
                continue;
            }
            if (open && c == open) ++depth;
            else if (c == close) {
                if (depth == 0) {
                    out += c;
                    ++i;
                    return;
                }
                --depth;
            }
            if (c == '\n') ++line;
            out += c;
            ++i;
        }
    }

    void block_params() {
        size_t k = i;
        while (k < n && blank(s[k])) ++k;
        if (k < n && s[k] == '|' && !(k + 1 < n && s[k + 1] == '|')) {
            size_t e = s.find('|', k + 1);
            if (e == std::string::npos) return;
            std::string p = s.substr(k + 1, e - k - 1);
            record_idents(p);
            copy(e + 1);
        }
    }
    void record_idents(const std::string& p) {
        size_t k = 0;
        bool in_default = false;
        int depth = 0;
        while (k < p.size()) {
            char c = p[k];
            if (c == '(' || c == '[' || c == '{') ++depth;
            if (c == ')' || c == ']' || c == '}') --depth;
            if (c == '=' && depth == 0) in_default = true;
            if (c == ',' && depth == 0) in_default = false;
            if (ident_start(c)) {
                size_t e = k;
                while (e < p.size() && ident_char(p[e])) ++e;
                if (!in_default) known.locals.insert(p.substr(k, e - k));
                k = e;
                continue;
            }
            ++k;
        }
    }

    // Is the identifier at i..j followed by an assignment?
    bool assigned(size_t j) const {
        size_t k = j;
        while (k < n && blank(s[k])) ++k;
        if (k >= n) return false;
        if (s[k] == '=' && !(k + 1 < n && (s[k + 1] == '=' || s[k + 1] == '~' || s[k + 1] == '>'))) return true;
        for (const char* op : {"+=", "-=", "*=", "/=", "%=", "||=", "&&=", "|=", "&=", "<<=", ">>=", "**=", "^="})
            if (s.compare(k, std::strlen(op), op) == 0) return true;
        return false;
    }

    char close_char() const {
        char f = frames.back();
        return f == 'G' ? ')' : f == 'B' ? '}' : 0;
    }

    void command(size_t from, bool value_mode) {
        int ln = line;
        CmdParser p(s, from, n, close_char(), file, ln, known, res);
        p.inline_end = inline_kw;
        Line L;
        L.file = file;
        L.line = ln;
        // here-doc fix-up needs item/cmd numbers: they are known when parse() ends, recorded below.
        bool ok = p.parse(L);
        if (!ok) {
            error(ln + p.nl, p.err);
            size_t e = s.find('\n', p.i);
            i = e == std::string::npos ? n : e;
            line += p.nl;
            out += "nil";
            return;
        }
        bool tail = p.tail != std::string::npos;
        L.text = first_line(s.substr(from, (tail ? p.tail - (p.tail_colon ? 1 : 0) : p.i) - from));
        if (tail) {  // the text shown in messages ends with the last command, not with || or &&
            while (!L.text.empty() && (blank(L.text.back()) || L.text.back() == '|' || L.text.back() == '&')) L.text.pop_back();
            value_mode = true;
        }
        int idx = int(table().size());
        table().push_back(std::move(L));
        std::string call = std::string(p.do_block ? "__rsh_cd_do(" : "__rsh_cmd(") + std::to_string(idx) + ", [";
        for (size_t k = 0; k < p.vals.size(); ++k) call += (k ? ", " : "") + p.vals[k];
        call += "]";
        if (value_mode) call += ", true";
        call += ")";
        if (p.do_block) call += " do";
        if (tail) call = "(" + call + (p.tail_or ? ") or " : ") and ") + (p.tail_colon ? "nil" : "");
        out += call;
        int pad = p.nl - count_nl(call);
        for (int k = 0; k < pad; ++k) out += '\n';
        line += p.nl;
        if (p.do_block) ++kw;
        res.marks.push_back({ln, 'C', table()[idx].text});
        hints(table()[idx], ln);
        if (tail) {  // Ruby goes on: `exit 1`, `raise "x" unless y`
            i = p.tail;
            stmt = false;
            prev = p.tail_colon ? Value : Op;
            return;
        }
        i = p.i;
        if (p.do_block || (i > 0 && s[i - 1] == '\n')) {
            stmt = true;
            prev = Start;
        } else {
            stmt = false;
            prev = Value;
        }
    }

    // Warnings for -n and --lint about a command line that may have been meant as Ruby (0.1.4).
    void hints(const Line& L, int ln) {
        if (forced_now || L.items.empty() || L.items[0].pipeline.empty()) return;
        const Cmd& c = L.items[0].pipeline[0];
        if (c.words.empty() || !c.env.empty()) return;
        auto plain = [](const Word& w) -> std::string {
            return w.parts.size() == 1 && w.parts[0].k == Part::Pat ? w.parts[0].s : std::string();
        };
        std::string w0 = plain(c.words[0]);
        if (w0.empty() || !ident_start(w0[0])) return;
        std::string w1 = c.words.size() > 1 ? plain(c.words[1]) : "";
        bool op = false;
        for (const char* o : {"==", "!=", "<=>", "===", "=~", "!~", "+", "..", "...", "=>", "%", "**"}) op |= w1 == o;
        bool single = L.items.size() == 1 && L.items[0].pipeline.size() == 1 && c.words.size() == 1 && c.redirs.empty();
        if (frames.back() == 'G' && (single || op))  // audit R2-3: x = 1 + (y), total = (count + 1) * 2
            warning(ln, "`" + w0 + "` inside ( ) is run as a command (a statement starts after a grouping `(`); if it is Ruby, write " + w0 +
                            "() or declare it with rsh_methods :" + w0 + "; ($ " + w0 + " ...) says it is a command");
        else if (op)  // audit R2-12: to_der == other.to_der, foo + 1
            warning(ln, "`" + w1 + "` is the first argument of the command `" + w0 + "`; if this is a Ruby expression, write " + w0 +
                            "() " + w1 + " ... or declare it with rsh_methods :" + w0);
    }

    // sh forms that are not rsh, at a statement start: a reader error that says what to write
    // instead of the Ruby syntax error they gave (0.1.4, audit L-16, R2-20).
    bool sh_hint() {
        if (plain_rest || !regions.empty()) return false;
        size_t e = s.find('\n', i);
        if (e == std::string::npos) e = n;
        std::string ln = s.substr(i, e - i);
        auto starts = [&](const char* t) { return ln.compare(0, std::strlen(t), t) == 0; };
        std::vector<std::string> tok;
        for (size_t k = 0; k < ln.size();) {
            while (k < ln.size() && blank(ln[k])) ++k;
            size_t b = k;
            while (k < ln.size() && !blank(ln[k])) ++k;
            if (k > b) tok.push_back(ln.substr(b, k - b));
        }
        std::string why;
        if ((starts("[ ") || starts("[[ ")) && tok.size() >= 3) {
            bool closed = false, shape = false, comma = false;  // a Ruby Array has commas: [ "aix", $1 ]
            for (size_t k = 1; k < tok.size(); ++k) {
                const std::string& t = tok[k];
                if (t == "]" || t == "]]" || t == "];" || t == "]];") closed = true;
                if (!closed && t.find(',') != std::string::npos) comma = true;
                if (!closed && (t == "=" || t == "!=" || t == "==" || t == "-eq" || t == "-ne" || t == "-lt" || t == "-le" ||
                                t == "-gt" || t == "-ge" || t[0] == '$' || t.compare(0, 2, "\"$") == 0))
                    shape = true;
                if (k == 1 && t.size() >= 2 && t[0] == '-' && std::isalpha((unsigned char)t[1])) shape = true;
            }
            if (closed && shape && !comma)
                why = "`[ ... ]` is sh: write the test in Ruby (File.file?(f), File.directory?(d), a == b, s.empty?) or run the program: test -f f";
        } else if (ident_start(ln[0])) {
            size_t k = 0;
            while (k < ln.size() && ident_char(ln[k])) ++k;
            std::string w = ln.substr(0, k);
            if (ln.compare(k, 3, "=$(") == 0)
                why = "`" + w + "=$(command)` is sh: write " + w + " = `command` (a capture; its status is in $?)";
            else if (w == "for" && tok.size() >= 4 && tok[2] == "in" && (ln.find("; do") != std::string::npos || ln.find(";do") != std::string::npos))
                why = "a sh for loop: write for " + tok[1] + " in list ... end (a list in Ruby: %w[a b], Dir.glob(\"*.txt\"), ARGV)";
            else if (w == "case" && tok.size() >= 3 && tok.back() == "in")
                why = "a sh case: write case " + tok[1] + " / when \"a\" then ... / end";
            else if (w == "set" && tok.size() >= 2 && tok[1].size() >= 2 && (tok[1][0] == '-' || tok[1][0] == '+') &&
                     std::isalpha((unsigned char)tok[1][1]) && command_at(i))
                why = tok[1].find('x') != std::string::npos
                          ? "`set " + tok[1] + "` is sh: rsh has no trace mode (rsh --explain shows how each line is read); it already stops at a failed command"
                          : "`set " + tok[1] + "` is sh: rsh already stops at a failed command (relaxed! turns that off) and an unset $NAME is an error";
            else if (w == "alias" && tok.size() >= 2 && tok[1].find('=') != std::string::npos && tok[1][0] != '=')
                why = "`alias " + tok[1].substr(0, tok[1].find('=')) + "=...` is sh: write a method, def " + tok[1].substr(0, tok[1].find('=')) +
                      "(*args) with the command line inside (#{args} passes the arguments), end";
            else if ((w == "true" || w == "false") && tok.size() >= 4 && (tok[1] == "&&" || tok[1] == "||")) {
                size_t q = ln.find(tok[1]) + 2;
                while (q < ln.size() && blank(ln[q])) ++q;
                size_t qe = q;
                while (qe < ln.size() && ident_char(ln[qe])) ++qe;
                bool arg = qe < ln.size() && blank(ln[qe]);
                if (q < ln.size() && command_at(i + q) && arg)
                    why = "`" + w + "` is Ruby's " + w + " here, so the line is Ruby: write the command alone, or $ " + w + " " + tok[1] + " ...";
            }
        } else if (starts(". ") && tok.size() >= 2) {
            const std::string& f = tok[1];
            bool file = f.find('/') != std::string::npos || f[0] == '$' || f[0] == '"';
            for (const char* x : {".sh", ".rsh", ".rb", "rc", ".env"})
                file |= f.size() > std::strlen(x) && f.compare(f.size() - std::strlen(x), std::strlen(x), x) == 0;
            if (file) why = "`. " + f + "` is sh: write source " + f;
        }
        if (why.empty()) return false;
        error(line, why);
        out += "nil";
        i = e;
        stmt = false;
        prev = Value;
        return true;
    }

    // `...` or %x{...}: one command line, captured.
    void capture(char close, char open) {
        int ln = line;
        size_t k = i;
        int depth = 0;
        while (k < n) {
            if (s[k] == '\\') {
                k += 2;
                continue;
            }
            if (s[k] == '#' && k + 1 < n && s[k + 1] == '{') {
                size_t e = match_brace(s, k + 2, n);
                if (e == std::string::npos) break;
                k = e + 1;
                continue;
            }
            if (open && s[k] == open) ++depth;
            else if (s[k] == close) {
                if (depth == 0) break;
                --depth;
            }
            ++k;
        }
        if (k >= n) {
            error(ln, "a capture is not closed");
            i = n;
            return;
        }
        CmdParser p(s, i, k, 0, file, ln, known, res);
        p.capture = true;
        Line L;
        L.file = file;
        L.line = ln;
        bool ok = p.parse(L);
        if (ok && p.i < k) ok = p.fail("a capture holds one command line");
        if (!ok) {
            error(ln, p.err);
            out += "nil";
        } else {
            L.text = first_line(s.substr(i, k - i));
            int idx = int(table().size());
            table().push_back(std::move(L));
            std::string call = "__rsh_cap(" + std::to_string(idx) + ", [";
            for (size_t m = 0; m < p.vals.size(); ++m) call += (m ? ", " : "") + p.vals[m];
            call += "])";
            out += call;
            int pad = count_nl(s.substr(i, k - i)) - count_nl(call);
            for (int m = 0; m < pad; ++m) out += '\n';
            res.marks.push_back({ln, 'X', table()[idx].text});
        }
        line = ln + count_nl(s.substr(i, k - i));
        i = k + 1;
        prev = Value;
    }

    // Does a command line start at `p`? The rule of docs/rsh.md, "How a line is read"; it looks only
    // at the text. `at_bang` is set for `! cmd` (sh negation, audit M-4): the word after `!` decides.
    bool command_at(size_t p, bool at_bang = false) const {
        if (s.compare(p, 2, "./") == 0 || s.compare(p, 3, "../") == 0 ||
            (s[p] == '~' && p + 1 < n && (s[p + 1] == '/' || ident_start(s[p + 1]))))
            return true;
        if (s[p] == '/' && p + 1 < n && (ident_char(s[p + 1]) || s[p + 1] == '.')) {
            size_t e = p;
            while (e < n && !blank(s[e]) && s[e] != '\n') ++e;
            std::string w = s.substr(p, e - p);
            return w.back() != '/' && w.find("/.") == std::string::npos && w.find("=~") == std::string::npos;
        }
        if (!at_bang && s[p] == '!' && p + 1 < n && blank(s[p + 1])) {
            size_t k = p + 1;
            while (k < n && blank(s[k])) ++k;
            return k < n && s[k] != '\n' && command_at(k, true);
        }
        if (!ident_start(s[p])) return false;
        size_t j = p;
        while (j < n && ident_char(s[j])) ++j;
        std::string w = s.substr(p, j - p);
        if (j < n && (s[j] == '?' || s[j] == '!')) return false;
        if (is_keyword(w)) return false;
        if (std::isupper((unsigned char)w[0])) {
            // NAME=value command ...: the value is one shell word and a command word follows it
            // (0.1.4: `P=Struct.new(:x) do`, `P=1; puts 1`, `P=f(1) + 2` are Ruby, audit R2-4).
            if (j < n && s[j] == '=' && j + 1 < n && !std::strchr("=~ \t\n", s[j + 1])) {
                size_t k = j + 1;
                char q = 0;
                for (; k < n && (q || (!blank(s[k]) && s[k] != '\n')); ++k) {
                    char c = s[k];
                    if (q) {
                        if (c == '\\' && q == '"') ++k;
                        else if (c == q) q = 0;
                        continue;
                    }
                    if (c == '"' || c == '\'') q = c;
                    else if (c == '\\') ++k;
                    else if (c == '$' && k + 1 < n && s[k + 1] == '{') {
                        size_t e = s.find('}', k);
                        if (e == std::string::npos) return false;
                        k = e;
                    } else if (c == '#' && k + 1 < n && s[k + 1] == '{') {
                        size_t e = match_brace(s, k + 2, n);
                        if (e == std::string::npos) return false;
                        k = e;
                    } else if (std::strchr("()[]{};", c) || (c == ':' && k + 1 < n && s[k + 1] == ':'))
                        return false;
                }
                if (q) return false;
                while (k < n && blank(s[k])) ++k;
                if (k >= n || s[k] == '\n' || s[k] == '#' || s[k] == ';') return false;
                if (s[k] == '$' && k + 1 < n && blank(s[k + 1])) return true;
                if (!ident_start(s[k]) && !std::strchr("./~\"'", s[k])) return false;
                if (ident_start(s[k])) {
                    size_t e = k;
                    while (e < n && ident_char(s[e])) ++e;
                    if (is_keyword(s.substr(k, e - k))) return false;  // P=x if y, P=x rescue nil
                }
                return true;
            }
            return false;
        }
        if (known.locals.count(w) || known.methods.count(w) || is_ruby_word(w)) return false;
        if (j < n && std::strchr(".([,=:", s[j])) return false;
        if (assigned(j)) return false;
        size_t k = j;
        while (k < n && blank(s[k])) ++k;
        if (s.compare(k, 2, "do") == 0 && (k + 2 >= n || !ident_char(s[k + 2]))) return false;
        // `foo { ... }` is a block; `echo {a,b}` is a word with a brace pattern (0.1.4, audit M-1):
        // a `,` inside, no blank, no `|` after the `{`, closed on the same line.
        if (k < n && s[k] == '{' && !brace_word(k)) return false;
        return true;
    }
    bool brace_word(size_t k) const {
        if (k + 1 >= n || blank(s[k + 1]) || s[k + 1] == '|' || s[k + 1] == '}' || s[k + 1] == '\n') return false;
        int depth = 0;
        bool comma = false;
        for (size_t e = k; e < n; ++e) {
            char c = s[e];
            if (blank(c) || c == '\n' || c == '"' || c == '\'') return false;
            if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) return comma;
            else if (c == ',' && depth == 1) comma = true;
        }
        return false;
    }

    bool try_command() {
        bool value_mode = cond || frames.back() == 'G';
        bool forced = s[i] == '$' && i + 1 < n && blank(s[i + 1]);
        if ((plain_rest || !regions.empty()) && !forced) return false;
        if (forced) {
            i += 2;
            while (i < n && blank(s[i])) ++i;
            forced_now = true;
            command(i, value_mode);
            forced_now = false;
            return true;
        }
        if (!command_at(i)) return false;
        command(i, value_mode);
        return true;
    }

    bool regex_ok(bool space_before) const {
        if (prev != Value) return true;
        return space_before && i + 1 < n && s[i + 1] != ' ' && s[i + 1] != '=';
    }

    void code(bool interp) {
        size_t depth0 = frames.size();
        while (i < n) {
            char c = s[i];
            bool bol = i == 0 || s[i - 1] == '\n';
            if (bol && !interp) {
                if (at("=begin") && (i + 6 >= n || std::isspace((unsigned char)s[i + 6]))) {
                    size_t e = s.find("\n=end", i);
                    e = e == std::string::npos ? n : s.find('\n', e + 1);
                    copy(e == std::string::npos ? n : e);
                    continue;
                }
                if (at("__END__") && (i + 7 == n || s[i + 7] == '\n')) {
                    copy(n);
                    return;
                }
            }
            if (blank(c)) {
                out += c;
                ++i;
                space = true;
                continue;
            }
            if (c == '\\' && i + 1 < n && s[i + 1] == '\n') {
                copy(i + 2);
                space = true;
                continue;
            }
            if (c == '\n') {
                out += '\n';
                ++i;
                ++line;
                space = true;
                flush_heredocs();
                char f = frames.back();
                if ((f == 'T' || f == 'B' || f == 'G') && prev != Op) {
                    stmt = true;
                    cond = false;
                }
                rescue_line = false;
                loop_do = false;
                pending_ruby = false;
                inline_kw = false;
                continue;
            }
            bool sp = space;
            space = false;
            if (c == '#' && !(i + 1 < n && s[i + 1] == '{')) {
                size_t e = s.find('\n', i);
                copy(e == std::string::npos ? n : e);
                space = true;
                continue;
            }
            if (c == ';') {
                out += ';';
                ++i;
                loop_do = false;
                stmt = true;
                cond = false;
                prev = Start;
                continue;
            }
            // `#{x} hi` at a statement start is a Ruby comment: nothing runs (0.9.1, audit 2 M11)
            if (stmt && !interp && c == '#' && i + 1 < n && s[i + 1] == '{')
                warning(line, "a line that starts with #{...} is a Ruby comment and runs nothing; to run the value as a command write $ #{...}");
            if ((stmt || cond) && !interp && (sh_hint() || try_command())) {
                cond = false;
                continue;
            }
            bool at_start = stmt;
            stmt = false;
            cond = false;
            bool dot = after_dot;
            after_dot = false;
            bool lam = lambda;
            lambda = false;

            if (c == '"') {
                out += c;
                ++i;
                dstring('"', 0);
                prev = Value;
                continue;
            }
            if (c == '\'') {
                size_t k = i + 1;
                while (k < n && s[k] != '\'') k += s[k] == '\\' ? 2 : 1;
                copy(k < n ? k + 1 : n);
                prev = Value;
                continue;
            }
            if (c == '`') {
                ++i;
                capture('`', 0);
                continue;
            }
            if (ident_start(c)) {
                size_t j = i;
                while (j < n && ident_char(s[j])) ++j;
                if (j < n && (s[j] == '?' || s[j] == '!') && !(j + 1 < n && s[j + 1] == '=' && !(j + 2 < n && s[j + 2] == '=')))
                    ++j;
                std::string w = s.substr(i, j - i);
                if (dot) {
                    copy(j);
                    prev = Value;
                    continue;
                }
                if (j < n && s[j] == ':' && !(j + 1 < n && s[j + 1] == ':')) {  // label
                    copy(j + 1);
                    prev = Op;
                    continue;
                }
                if (is_keyword(w)) {
                    Prev before = prev;
                    copy(j);
                    if (w == "class" || w == "module" || w == "case" || w == "begin") ++kw;
                    else if (w == "if" || w == "unless" || w == "while" || w == "until") {
                        if (before != Value || at_start) {  // not a modifier
                            ++kw;
                            if (w == "while" || w == "until") loop_do = true;
                        }
                    } else if (w == "for") ++kw, loop_do = true;
                    else if (w == "do") {
                        if (loop_do) loop_do = false;
                        else ++kw;
                        if (pending_ruby) {
                            regions.push_back({kw, 0, pending_line});
                            pending_ruby = false;
                        }
                    } else if (w == "end") {
                        --kw;
                        if (!regions.empty() && regions.back().frame == 0 && kw < regions.back().kw) {
                            res.ruby_spans.push_back({regions.back().line, line});
                            regions.pop_back();
                        }
                    }
                    if (w == "def") {
                        size_t k = i;
                        while (k < n && blank(s[k])) ++k;
                        if (s.compare(k, 5, "self.") == 0) k += 5;
                        size_t b = k;
                        while (k < n && (ident_char(s[k]) || s[k] == '?' || s[k] == '!' || s[k] == '=')) ++k;
                        if (k == b)  // an operator: def <=>(o), def ==(o), def [](i), def -@
                            while (k < n && std::strchr("+-*/%<=>!~^&|[]@", s[k])) ++k;
                        if (k > b && ident_start(s[b])) known.methods.insert(s.substr(b, k - b));
                        {  // `def name(args) = expr` has no `end`
                            size_t q = k;
                            while (q < n && blank(s[q])) ++q;
                            if (q < n && s[q] == '(') {
                                int d = 0;
                                for (; q < n && s[q] != '\n'; ++q) {
                                    if (s[q] == '(') ++d;
                                    else if (s[q] == ')' && --d == 0) break;
                                }
                                if (q < n && s[q] == ')') ++q;
                                while (q < n && blank(s[q])) ++q;
                            }
                            bool endless = q + 1 < n && s[q] == '=' && !std::strchr("=~>", s[q + 1]);
                            if (!endless) ++kw;
                        }
                        size_t e = s.find('\n', k);
                        std::string params = s.substr(k, (e == std::string::npos ? n : e) - k);
                        {  // `def f; body; end` on one line: the parameters end at the first `;`
                            int d = 0;
                            for (size_t q = 0; q < params.size(); ++q) {
                                if (params[q] == '(') ++d;
                                else if (params[q] == ')') --d;
                                else if (params[q] == ';' && d <= 0) {
                                    params.resize(q);
                                    break;
                                }
                            }
                        }
                        record_idents(params);
                        copy(k);
                        prev = Value;
                    } else if (w == "if" || w == "unless" || w == "while" || w == "until" || w == "elsif") {
                        prev = Op;
                        cond = true;
                    } else if (w == "then" || w == "else" || w == "begin" || w == "ensure") {
                        prev = Start;
                        stmt = true;
                        inline_kw = true;
                    } else if (w == "do") {
                        block_params();
                        prev = Start;
                        stmt = true;
                        inline_kw = true;
                    } else if (w == "for") {
                        size_t k = i;
                        while (k < n && blank(s[k])) ++k;
                        size_t b = k;
                        while (k < n && ident_char(s[k])) ++k;
                        known.locals.insert(s.substr(b, k - b));
                        prev = Op;
                    } else if (w == "rescue") {
                        rescue_line = true;
                        prev = Value;
                    } else if (w == "and" || w == "or" || w == "not" || w == "in" || w == "when" || w == "case" ||
                               w == "class" || w == "module" || w == "alias" || w == "undef") {
                        prev = Op;
                    } else prev = Value;
                    continue;
                }
                if (at_start && w == "ruby" && !known.locals.count(w)) {
                    size_t k = j;
                    while (k < n && blank(s[k])) ++k;
                    if ((s.compare(k, 2, "do") == 0 && (k + 2 >= n || !ident_char(s[k + 2]))) || (k < n && s[k] == '{')) {
                        pending_ruby = true;
                        pending_line = line;
                    }
                }
                if (at_start && w == "ruby!" && !plain_rest) {
                    size_t k = j;
                    while (k < n && blank(s[k])) ++k;
                    if (k >= n || s[k] == '\n' || s[k] == '#' || s[k] == ';') {
                        plain_rest = true;
                        res.ruby_spans.push_back({line, 0});
                    }
                }
                if (w == "define_method" || w == "define_singleton_method" || w == "alias_method" || w == "rsh_methods" ||
                    w == "attr_reader" || w == "attr_accessor" || w == "attr_writer") {  // names for the lines after
                    size_t e = s.find('\n', i);
                    declared_names(s.substr(i, (e == std::string::npos ? n : e) - i), known.methods);
                }
                if (assigned(j)) known.locals.insert(w);
                if (rescue_line && out.size() >= 2) {
                    size_t k = out.size();
                    while (k > 0 && blank(out[k - 1])) --k;
                    if (k >= 2 && out.compare(k - 2, 2, "=>") == 0) known.locals.insert(w);
                }
                copy(j);
                prev = Value;
                continue;
            }
            if (std::isdigit((unsigned char)c)) {
                size_t j = i;
                while (j < n && (ident_char(s[j]) || (s[j] == '.' && j + 1 < n && std::isdigit((unsigned char)s[j + 1])))) ++j;
                copy(j);
                prev = Value;
                continue;
            }
            if (c == '$' && i + 1 < n) {
                size_t j = i + 1;
                if (ident_start(s[j]) || std::isdigit((unsigned char)s[j]))
                    while (j < n && ident_char(s[j])) ++j;
                else ++j;
                copy(j);
                prev = Value;
                continue;
            }
            if (c == '@') {
                size_t j = i + 1;
                if (j < n && s[j] == '@') ++j;
                while (j < n && ident_char(s[j])) ++j;
                copy(j);
                prev = Value;
                continue;
            }
            if (c == ':') {
                if (i + 1 < n && s[i + 1] == ':') {
                    copy(i + 2);
                    after_dot = true;
                    prev = Op;
                } else if (i + 1 < n && s[i + 1] == '"') {
                    out += ":\"";
                    i += 2;
                    dstring('"', 0);
                    prev = Value;
                } else if (i + 1 < n && ident_start(s[i + 1])) {
                    size_t j = i + 1;
                    while (j < n && ident_char(s[j])) ++j;
                    if (j < n && (s[j] == '?' || s[j] == '!' || s[j] == '=')) ++j;
                    copy(j);
                    prev = Value;
                } else {
                    copy(i + 1);
                    prev = Op;
                }
                continue;
            }
            if (c == '%' && i + 1 < n && (prev != Value || (sp && s[i + 1] != ' ' && s[i + 1] != '='))) {
                size_t j = i + 1;
                char type = 0;
                if (j < n && std::strchr("qQwWiIrsx", s[j]) && j + 1 < n && !std::isalnum((unsigned char)s[j + 1]) &&
                    !std::isspace((unsigned char)s[j + 1]))
                    type = s[j++];
                char open = j < n ? s[j] : 0;
                if (open && !std::isalnum((unsigned char)open) && !std::isspace((unsigned char)open)) {
                    char close = open == '(' ? ')' : open == '[' ? ']' : open == '{' ? '}' : open == '<' ? '>' : open;
                    char nest = close == open ? 0 : open;
                    if (type == 'x') {
                        i = j + 1;
                        capture(close, nest);
                        continue;
                    }
                    copy(j + 1);
                    dstring(close, nest);
                    if (type == 'r')
                        while (i < n && std::isalpha((unsigned char)s[i])) copy(i + 1);
                    prev = Value;
                    continue;
                }
            }
            if (c == '/' && regex_ok(sp)) {
                out += '/';
                ++i;
                dstring('/', 0);
                while (i < n && std::isalpha((unsigned char)s[i])) copy(i + 1);
                prev = Value;
                continue;
            }
            if (c == '?' && prev != Value && i + 1 < n && !std::isspace((unsigned char)s[i + 1])) {
                size_t j = i + 1;
                if (s[j] == '\\') j += 2;
                else ++j;
                if (j >= n || !ident_char(s[j])) {
                    copy(j);
                    prev = Value;
                    continue;
                }
            }
            if (c == '<' && i + 2 < n && s[i + 1] == '<' && (prev != Value || (sp && s[i + 2] != ' '))) {
                size_t k = i + 2;
                bool dash = s[k] == '~' || s[k] == '-';
                if (dash) ++k;
                char q = (s[k] == '\'' || s[k] == '"' || s[k] == '`') ? s[k] : 0;
                size_t b = q ? k + 1 : k;
                size_t e = b;
                while (e < n && (q ? s[e] != q : ident_char(s[e]))) ++e;
                if (e > b && (q || ident_start(s[b]))) {
                    heredocs.push_back({s.substr(b, e - b), dash});
                    copy(q ? e + 1 : e);
                    prev = Value;
                    continue;
                }
            }
            if (c == '{') {
                bool block = (prev == Value && !at_start) || lam;
                frames.push_back(block ? 'B' : 'H');
                copy(i + 1);
                if (block && pending_ruby) {
                    regions.push_back({kw, frames.size(), pending_line});
                    pending_ruby = false;
                }
                if (block) {
                    block_params();
                    prev = Start;
                    stmt = true;
                } else prev = Op;
                continue;
            }
            if (c == '}') {
                if (interp && frames.size() == depth0) return;
                if (!regions.empty() && regions.back().frame == frames.size()) {
                    res.ruby_spans.push_back({regions.back().line, line});
                    regions.pop_back();
                }
                if (frames.size() > 1) frames.pop_back();
                copy(i + 1);
                prev = Value;
                continue;
            }
            if (c == '(' && lam) {  // ->(a, b = 1) { ... }: parameters, then the body
                size_t k = i + 1;
                int d = 1;
                for (; k < n && d; ++k) d += s[k] == '(' ? 1 : s[k] == ')' ? -1 : 0;
                record_idents(s.substr(i + 1, k - i - 2));
                copy(k);
                lambda = true;
                prev = Op;
                continue;
            }
            if (c == '(') {
                bool call = prev == Value && !at_start;  // at a statement start it groups
                frames.push_back(call ? 'P' : 'G');
                copy(i + 1);
                if (!call) {
                    stmt = true;
                    prev = Start;
                } else prev = Op;
                continue;
            }
            if (c == '[') {
                frames.push_back('P');
                copy(i + 1);
                prev = Op;
                continue;
            }
            if (c == ')' || c == ']') {
                if (frames.size() > 1) frames.pop_back();
                copy(i + 1);
                prev = Value;
                continue;
            }
            if (c == '-' && i + 1 < n && s[i + 1] == '>') {
                copy(i + 2);
                prev = Op;
                lambda = true;
                continue;
            }
            if (c == '.' || (c == '&' && i + 1 < n && s[i + 1] == '.')) {
                if (c == '.' && i + 1 < n && s[i + 1] == '.') {  // range
                    copy(i + 2);
                    prev = Op;
                    continue;
                }
                copy(i + (c == '&' ? 2 : 1));
                after_dot = true;
                prev = Op;
                continue;
            }
            copy(i + 1);
            prev = Op;
        }
    }
};

std::string fragment(const std::string& src, const std::string& file, int line, Known& known, Result& res) {
    Tx t(src, file, line, known, res);
    t.stmt = false;
    t.prev = Tx::Op;
    // A #{...} value is an expression, as in a Ruby string: `echo #{name}` calls the method `name`.
    t.plain_rest = !src.empty() && src[0] == '(';
    t.code(false);
    return t.out;
}

}  // namespace

// The method names written after a declaring word, from `ln[k]` on: `:a, :b`, `"a"`, `'a'`,
// `%i[a b]`, `%w[a b]`, with or without parentheses; with `bare` also a bare word (`alias new old`;
// elsewhere a bare word is a variable, not a name). With `first` only the first name counts
// (define_method :name, alias_method :new, :old).
void literal_names(const std::string& ln, size_t k, std::set<std::string>& out, bool first, bool bare) {
    auto name_ok = [](const std::string& n) { return !n.empty() && ident_start(n[0]) && !std::isupper((unsigned char)n[0]); };
    while (k < ln.size() && (blank(ln[k]) || ln[k] == '(')) ++k;
    while (k < ln.size()) {
        std::string n;
        char c = ln[k];
        if (c == ':' && k + 1 < ln.size() && ident_start(ln[k + 1])) {
            size_t b = ++k;
            while (k < ln.size() && ident_char(ln[k])) ++k;
            if (k < ln.size() && (ln[k] == '?' || ln[k] == '!')) ++k;
            n = ln.substr(b, k - b);
        } else if (c == '"' || c == '\'') {
            size_t e = ln.find(c, k + 1);
            if (e == std::string::npos) return;
            n = ln.substr(k + 1, e - k - 1);
            if (n.find("#{") != std::string::npos) return;
            k = e + 1;
        } else if (c == '%' && k + 2 < ln.size() && (ln[k + 1] == 'i' || ln[k + 1] == 'w') &&
                   std::strchr("[({<", ln[k + 2])) {
            char open = ln[k + 2];
            char close = open == '[' ? ']' : open == '(' ? ')' : open == '{' ? '}' : '>';
            size_t e = ln.find(close, k + 3);
            if (e == std::string::npos) return;
            std::string body = ln.substr(k + 3, e - k - 3);
            size_t b = 0;
            while (b < body.size()) {
                while (b < body.size() && blank(body[b])) ++b;
                size_t f = b;
                while (f < body.size() && !blank(body[f])) ++f;
                std::string w = body.substr(b, f - b);
                if (name_ok(w)) out.insert(w);
                b = f;
            }
            k = e + 1;
            if (first) return;
        } else if (ident_start(c)) {
            size_t b = k;
            while (k < ln.size() && ident_char(ln[k])) ++k;
            if (k < ln.size() && (ln[k] == '?' || ln[k] == '!')) ++k;
            n = ln.substr(b, k - b);
            if (!bare) return;  // a variable: not a literal name
        } else return;
        if (name_ok(n)) out.insert(n);
        if (first) return;
        while (k < ln.size() && blank(ln[k])) ++k;
        if (k >= ln.size() || ln[k] != ',') return;
        ++k;
        while (k < ln.size() && blank(ln[k])) ++k;
    }
}

// Words after which literal names are methods the script makes at run time.
struct Declarer {
    const char* word;
    bool first;  // only the first name
};
static const Declarer DECLARERS[] = {
    {"define_method", true},  {"define_singleton_method", true}, {"alias_method", true}, {"alias", true},
    {"attr_reader", false},   {"attr_accessor", false},          {"attr_writer", false}, {"rsh_methods", false},
    {"Struct.new", false},    {"Data.define", false},            {nullptr, false}};

// Every declaring word in one line of text (not only at its start), up to a comment.
void declared_names(const std::string& line, std::set<std::string>& out) {
    std::string ln = line;
    char q = 0;
    for (size_t k = 0; k < ln.size(); ++k) {
        char c = ln[k];
        if (q) {
            if (c == '\\') ++k;
            else if (c == q) q = 0;
        } else if (c == '"' || c == '\'') q = c;
        else if (c == '#' && !(k + 1 < ln.size() && ln[k + 1] == '{')) {
            ln.resize(k);
            break;
        }
    }
    for (const Declarer* d = DECLARERS; d->word; ++d) {
        size_t len = std::strlen(d->word);
        for (size_t at = ln.find(d->word); at != std::string::npos; at = ln.find(d->word, at + 1)) {
            if (at > 0 && (ident_char(ln[at - 1]) || ln[at - 1] == ':' || ln[at - 1] == '@' || ln[at - 1] == '$')) continue;
            size_t k = at + len;
            if (k < ln.size() && !blank(ln[k]) && ln[k] != '(') continue;
            if (!std::strcmp(d->word, "alias") && k < ln.size() && ln[k] == '(') continue;
            literal_names(ln, k, out, d->first, !std::strcmp(d->word, "alias"));
        }
    }
}

// The text of a file the reader reads ahead (require_relative, source): only a plain file, opened
// without blocking, so a FIFO, /dev/stdin or a device is never read at read time (0.9.0 review).
static bool read_ahead(const std::string& path, std::string& body) {
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return false;
    }
    char buf[8192];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) body.append(buf, size_t(r));
    close(fd);
    return true;
}

void collect_defs(const std::string& src, const std::string& dir, std::set<std::string>& out, int depth) {
    size_t i = 0;
    while (i < src.size()) {
        size_t e = src.find('\n', i);
        if (e == std::string::npos) e = src.size();
        std::string ln = trim(src.substr(i, e - i));
        i = e + 1;
        declared_names(ln, out);
        for (const char* mod : {"private ", "protected ", "public ", "module_function "})
            if (ln.compare(0, std::strlen(mod), mod) == 0) ln = trim(ln.substr(std::strlen(mod)));
        if (ln.compare(0, 4, "def ") == 0) {
            size_t k = 4;
            while (k < ln.size() && blank(ln[k])) ++k;
            if (ln.compare(k, 5, "self.") == 0) k += 5;
            size_t b = k;
            while (k < ln.size() && (ident_char(ln[k]) || ln[k] == '?' || ln[k] == '!')) ++k;
            if (k > b) out.insert(ln.substr(b, k - b));
        } else if (!dir.empty() && depth < 8 && ln.compare(0, 7, "source ") == 0) {
            // D3 (0.9.0): `source <literal path>` is read ahead for its method names, as
            // require_relative is. A path with #{}, $, a pattern or ~ is known only at run time.
            size_t k = 7;
            while (k < ln.size() && blank(ln[k])) ++k;
            std::string rel;
            if (k < ln.size() && (ln[k] == '"' || ln[k] == '\'')) {
                size_t qe = ln.find(ln[k], k + 1);
                if (qe == std::string::npos) continue;
                rel = ln.substr(k + 1, qe - k - 1);
                if (ln[k] == '"' && rel.find("#{") != std::string::npos) continue;
            } else {
                size_t b = k;
                while (k < ln.size() && !blank(ln[k]) && ln[k] != ';' && ln[k] != '#') ++k;
                rel = ln.substr(b, k - b);
                if (rel.find_first_of("$*?[{~\\`") != std::string::npos) continue;
            }
            if (rel.empty()) continue;
            // relative to this file's folder only, as `source` resolves a written-out path when it
            // runs (the CTO's decision): the same file reads the same on every machine
            std::string path = rel[0] == '/' ? rel : dir + "/" + rel;
            std::string body;
            if (!read_ahead(path, body)) continue;
            size_t slash = path.rfind('/');
            collect_defs(body, slash == std::string::npos ? "." : path.substr(0, slash), out, depth + 1);
        } else if (!dir.empty() && depth < 8 && ln.compare(0, 17, "require_relative ") == 0) {
            size_t q = ln.find_first_of("\"'", 17);
            if (q == std::string::npos) continue;
            size_t qe = ln.find(ln[q], q + 1);
            if (qe == std::string::npos) continue;
            std::string rel = ln.substr(q + 1, qe - q - 1);
            for (const char* ext : {"", ".rsh", ".rb"}) {
                std::string path = (rel[0] == '/' ? "" : dir + "/") + rel + ext;
                std::string body;
                if (!read_ahead(path, body)) continue;
                size_t slash = path.rfind('/');
                collect_defs(body, slash == std::string::npos ? "." : path.substr(0, slash), out, depth + 1);
                break;
            }
        }
    }
}

Result transpile(const std::string& src, const std::string& file, Known& known, int first_line) {
    Result res;
    Tx t(src, file, first_line, known, res);
    t.code(false);
    res.ruby = std::move(t.out);
    return res;
}

int parse_command(const std::string& text, const std::string& file, int line, std::vector<std::string>& values,
                  std::string& err) {
    Known k;
    Result r;
    CmdParser p(text, 0, text.size(), 0, file, line, k, r);
    p.capture = true;
    Line L;
    L.file = file;
    L.line = line;
    if (!p.parse(L) || p.i < text.size()) {
        err = p.err.empty() ? "one command line expected" : p.err;
        return -1;
    }
    L.text = first_line(text);
    values = p.vals;
    table().push_back(std::move(L));
    return int(table().size()) - 1;
}

}  // namespace rsh
