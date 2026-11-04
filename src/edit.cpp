// SPDX-License-Identifier: GPL-2.0-or-later
#include "edit.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace rsh {

namespace {

void out(const std::string& s) {
    size_t o = 0;
    while (o < s.size()) {
        ssize_t w = write(1, s.data() + o, s.size() - o);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        o += size_t(w);
    }
}

bool cont(unsigned char c) { return (c & 0xC0) == 0x80; }

size_t prev_char(const std::string& s, size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && cont((unsigned char)s[i])) --i;
    return i;
}
size_t next_char(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && cont((unsigned char)s[i])) ++i;
    return i;
}

int read_key() {
    unsigned char c;
    for (;;) {
        ssize_t n = read(0, &c, 1);
        if (n == 1) return c;
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
}

// More input is already there (a paste, keys typed ahead): the line is drawn once after it.
bool pending() {
    pollfd p{0, POLLIN, 0};
    return poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
}

enum Key { K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DEL, K_WLEFT, K_WRIGHT };

// After ESC. An Enter that follows is Enter, not part of a sequence (ESC, Enter swallowed the
// Enter, audit R2-10).
int escape() {
    int a = read_key();
    if (a == '\r' || a == '\n') return a;
    if (a == 'b') return K_WLEFT;
    if (a == 'f') return K_WRIGHT;
    if (a != '[' && a != 'O') return -2;
    int b = read_key();
    if (b == '\r' || b == '\n') return b;
    if (b >= '0' && b <= '9') {
        std::string num(1, char(b));
        int c;
        while ((c = read_key()) >= 0 && ((c >= '0' && c <= '9') || c == ';')) num += char(c);
        if (c == '~') {
            if (num == "3") return K_DEL;
            if (num == "1" || num == "7") return K_HOME;
            if (num == "4" || num == "8") return K_END;
        }
        if (num == "1;5" && c == 'D') return K_WLEFT;
        if (num == "1;5" && c == 'C') return K_WRIGHT;
        return -2;
    }
    switch (b) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
    }
    return -2;
}

int columns() {
    winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 80;
}

std::string common_prefix(const std::vector<std::string>& v) {
    if (v.empty()) return "";
    std::string p = v[0];
    for (auto& s : v) {
        size_t k = 0;
        while (k < p.size() && k < s.size() && p[k] == s[k]) ++k;
        p.resize(k);
    }
    return p;
}

}  // namespace

// Where the word being typed at `pos` starts: quotes and backslashes are followed, so `sp\ a` and
// `'sp a` are one word; | ; & < > ( and blanks outside quotes separate words.
size_t word_start(const std::string& line, size_t pos) {
    size_t start = 0;
    char q = 0;
    for (size_t k = 0; k < pos && k < line.size(); ++k) {
        char c = line[k];
        if (q) {
            if (c == '\\' && q == '"') ++k;
            else if (c == q) q = 0;
            continue;
        }
        if (c == '\\') {
            ++k;
            continue;
        }
        if (c == '\'' || c == '"') {
            q = c;
            continue;
        }
        if (c == ' ' || c == '\t' || std::strchr("|;&<>(", c)) start = k + 1;
    }
    return start;
}

// The text of a typed word without its quoting (an open quote is fine: `'sp a`).
std::string unquote_word(const std::string& w) {
    std::string o;
    char q = 0;
    for (size_t k = 0; k < w.size(); ++k) {
        char c = w[k];
        if (q == '\'') {
            if (c == '\'') q = 0;
            else o += c;
        } else if (c == '\\' && k + 1 < w.size() && (!q || std::strchr("\"\\$#`", w[k + 1]))) {
            o += w[++k];
        } else if (q == '"') {
            if (c == '"') q = 0;
            else o += c;
        } else if (c == '\'' || c == '"') q = c;
        else o += c;
    }
    return o;
}

// A name as one word of a command line (M-7, R2-16): a backslash before each character the reader
// would take as syntax; a name with a control byte (a tab, a newline) in single quotes. The bytes
// stay as they are (a name that is not UTF-8 is inserted as its own bytes).
std::string quote_word(const std::string& n, bool keep_tilde) {
    bool ctrl = false;
    for (char c : n) ctrl |= (unsigned char)c < 0x20 || c == 0x7f;
    if (ctrl) {
        std::string o = "'";
        for (char c : n) o += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return o + "'";
    }
    std::string o;
    for (size_t k = 0; k < n.size(); ++k) {
        char c = n[k];
        if (std::strchr(" ;|&<>()'\"\\$#*?[]{}`!", c) || (c == '~' && k == 0 && !keep_tilde)) o += '\\';
        o += c;
    }
    return o;
}

// Columns of one code point on a terminal: 0 for combining marks and joiners, 2 for East Asian
// wide and full-width forms and emoji, else 1 (audit L9; a table of the main blocks, no locale).
int char_width(uint32_t u) {
    struct R {
        uint32_t a, b;
    };
    static const R zero[] = {{0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A}, {0x064B, 0x065F},
                             {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F},
                             {0x20D0, 0x20FF}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xE0100, 0xE01EF}};
    static const R wide[] = {{0x1100, 0x115F}, {0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x25FD, 0x25FE}, {0x2614, 0x2615},
                             {0x2648, 0x2653}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5},
                             {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F5}, {0x26FA, 0x26FD}, {0x2705, 0x2705},
                             {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C}, {0x2753, 0x2755}, {0x2795, 0x2797},
                             {0x2B1B, 0x2B1C}, {0x2B50, 0x2B55}, {0x2E80, 0x303E}, {0x3041, 0x33FF}, {0x3400, 0x4DBF},
                             {0x4E00, 0x9FFF}, {0xA000, 0xA4CF}, {0xA960, 0xA97F}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF},
                             {0xFE10, 0xFE19}, {0xFE30, 0xFE6F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x1F004, 0x1F004},
                             {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F251},
                             {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F7E0, 0x1F7EB}, {0x1F90C, 0x1F9FF},
                             {0x1FA70, 0x1FAFF}, {0x20000, 0x3FFFD}};
    for (auto& r : zero)
        if (u >= r.a && u <= r.b) return 0;
    for (auto& r : wide)
        if (u >= r.a && u <= r.b) return 2;
    return 1;
}

size_t visible_width(const std::string& s) {
    size_t w = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) {  // CSI ... letter
            ++i;
            if (i < s.size() && s[i] == '[')
                while (i + 1 < s.size() && !std::isalpha((unsigned char)s[i + 1])) ++i;
            ++i;
            continue;
        }
        if (c < 0x80) {
            ++w;
            continue;
        }
        if (cont(c)) continue;
        size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
        uint32_t u = c & (n == 4 ? 0x07 : n == 3 ? 0x0F : 0x1F);
        size_t k = 1;
        for (; k < n && i + k < s.size() && cont((unsigned char)s[i + k]); ++k) u = (u << 6) | ((unsigned char)s[i + k] & 0x3F);
        w += k == n ? size_t(char_width(u)) : 1;
        i += k - 1;
    }
    return w;
}

std::string printable(const std::string& s) {
    static const char* hex = "0123456789abcdef";
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) {
            o += '^';
            o += c == 0x7f ? '?' : char(c + 64);
            ++i;
            continue;
        }
        if (c < 0x80) {
            o += char(c);
            ++i;
            continue;
        }
        size_t n = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        bool ok = n && i + n <= s.size();
        for (size_t k = 1; ok && k < n; ++k) ok = cont((unsigned char)s[i + k]);
        if (ok && c == 0xC2 && (unsigned char)s[i + 1] < 0xA0) ok = false;  // U+0080..U+009F: C1 controls
        if (!ok) {
            o += "\\x";
            o += hex[c >> 4];
            o += hex[c & 15];
            ++i;
            continue;
        }
        o.append(s, i, n);
        i += n;
    }
    return o;
}

bool keep_out_of_history(const std::string& line) {
    if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) return true;
    std::string up = line;
    for (char& c : up) c = char(std::toupper((unsigned char)c));
    // A secret word with an = or : after it: TOKEN=..., --password=..., ENV["API_KEY"] = ...
    for (const char* w : {"TOKEN", "SECRET", "PASSWORD", "PASSWD", "PASSPHRASE", "API_KEY", "APIKEY", "PRIVATE_KEY",
                          "ACCESS_KEY", "CREDENTIAL", "AUTHORIZATION:", "BEARER "}) {
        size_t p = up.find(w);
        if (p == std::string::npos) continue;
        size_t after = p + std::strlen(w);
        if (w[std::strlen(w) - 1] == ':' || w[std::strlen(w) - 1] == ' ') return true;
        if (up.find_first_of("=:", after) != std::string::npos) return true;
    }
    if (line.find("-----BEGIN ") != std::string::npos) return true;
    // Well-known token shapes, at the start of a word, with at least 8 more token characters.
    for (const char* t : {"ghp_", "gho_", "ghu_", "ghs_", "ghr_", "github_pat_", "glpat-", "xoxb-", "xoxp-", "xoxa-",
                          "xoxs-", "xapp-", "sk-ant-", "sk-proj-", "AKIA", "ASIA"}) {
        size_t n = std::strlen(t);
        for (size_t p = line.find(t); p != std::string::npos; p = line.find(t, p + 1)) {
            if (p > 0 && (std::isalnum((unsigned char)line[p - 1]) || line[p - 1] == '_')) continue;
            size_t k = p + n;
            while (k < line.size() && (std::isalnum((unsigned char)line[k]) || line[k] == '_' || line[k] == '-')) ++k;
            if (k - p - n >= (t[0] == 'A' ? 16u : 8u)) return true;
        }
    }
    return false;
}

static bool owner_ok(const struct stat& st, bool strict, std::string& why) {
    if (st.st_uid != geteuid()) {
        why = "it belongs to user " + std::to_string(st.st_uid);
        return false;
    }
    if (st.st_mode & S_IWOTH) {
        why = "others can write it";
        return false;
    }
    if (strict && (st.st_mode & S_IWGRP)) {
        why = "its group can write it";
        return false;
    }
    return true;
}

int open_own(const std::string& path, int flags, bool strict, std::string& why) {
    why.clear();
    int fd = open(path.c_str(), flags | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY, 0600);
    if (fd < 0) {
        if (errno == ELOOP) why = "it is a symbolic link";
        else if (errno != ENOENT) why = std::strerror(errno);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        why = "it is not a plain file";
        close(fd);
        return -1;
    }
    if (!owner_ok(st, strict, why)) {
        close(fd);
        return -1;
    }
    return fd;
}

bool own_dir(const std::string& path, bool strict, std::string& why) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {  // missing or not reachable: no file, nothing to warn about
        why.clear();
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        why = S_ISLNK(st.st_mode) ? "it is a symbolic link" : "it is not a folder";
        return false;
    }
    return owner_ok(st, strict, why);
}

void Editor::load() {
    if (history_file.empty()) return;
    std::string why;
    int fd = open_own(history_file, O_RDONLY, strict, why);
    if (fd < 0) {
        if (!why.empty()) {
            std::fprintf(stderr, "rsh: history is not kept in a file this time: %s: %s\n", printable(history_file).c_str(),
                         why.c_str());
            history_file.clear();
        }
        return;
    }
    FILE* f = fdopen(fd, "r");
    if (!f) {
        close(fd);
        return;
    }
    char buf[8192];
    while (std::fgets(buf, sizeof buf, f)) {
        std::string l(buf);
        while (!l.empty() && l.back() == '\n') l.pop_back();
        // a stored entry keeps its own newlines as \n
        std::string e;
        for (size_t k = 0; k < l.size(); ++k) {
            if (l[k] == '\\' && k + 1 < l.size()) {
                e += l[k + 1] == 'n' ? '\n' : l[k + 1];
                ++k;
            } else e += l[k];
        }
        if (!e.empty()) history.push_back(e);
    }
    std::fclose(f);
    if (history.size() > 5000) history.erase(history.begin(), history.end() - 5000);
}

void Editor::add(const std::string& entry) {
    if (entry.empty() || (!history.empty() && history.back() == entry)) return;
    if (keep_out_of_history(entry)) return;  // audit M4: not in memory, not in the file
    history.push_back(entry);
    if (history_file.empty()) return;
    std::string why;
    int fd = open_own(history_file, O_WRONLY | O_CREAT | O_APPEND, strict, why);
    if (fd < 0) {
        if (!why.empty()) {
            std::fprintf(stderr, "rsh: history is not kept in a file from now on: %s: %s\n", printable(history_file).c_str(),
                         why.c_str());
            history_file.clear();
        }
        return;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && (st.st_mode & 0777) != 0600) fchmod(fd, 0600);  // an older file keeps no other mode
    std::string l;
    for (char c : entry) {
        if (c == '\n') l += "\\n";
        else if (c == '\\') l += "\\\\";
        else l += c;
    }
    l += '\n';
    if (write(fd, l.data(), l.size()) < 0) {
    }
    close(fd);
}

bool Editor::read(const std::string& prompt, std::string& result) {
    if (!isatty(0)) {  // no terminal: plain lines
        out(prompt);
        std::string l;
        int c;
        while ((c = read_key()) >= 0 && c != '\n') l += char(c);
        if (c < 0 && l.empty()) return false;
        result = l;
        return true;
    }
    termios old, raw;
    tcgetattr(0, &old);
    raw = old;
    raw.c_iflag &= ~tcflag_t(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_lflag &= ~tcflag_t(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    // TCSADRAIN, not TCSAFLUSH: input not read yet (the rest of a pasted block, keys typed while a
    // command ran) stays for the next read (0.1.4, audit R2-2).
    tcsetattr(0, TCSADRAIN, &raw);

    std::string buf;
    size_t pos = 0;
    size_t hidx = history.size();
    std::string saved;
    size_t pw = visible_width(prompt);
    bool tabbed = false;

    auto draw = [&] {
        // A line with control bytes (a file name from Tab, an entry from history) is shown with
        // them made visible, without colours (audit M3).
        std::string safe = printable(buf);
        bool plain = safe == buf;
        std::string shown = !plain ? safe : paint ? paint(buf) : buf;
        size_t col = pw + visible_width(plain ? buf.substr(0, pos) : printable(buf.substr(0, pos)));
        std::string s = "\r" + prompt + shown + "\x1b[K\r";
        int cols = columns();
        if (col >= size_t(cols)) col = size_t(cols) - 1;
        if (col) s += "\x1b[" + std::to_string(col) + "C";
        out(s);
    };
    auto done = [&](bool ok) {
        pos = buf.size();
        draw();
        out("\r\n");
        tcsetattr(0, TCSADRAIN, &old);
        result = buf;
        return ok;
    };
    draw();
    int again = -1;  // a key the history search ended with, done here (Left, Ctrl-A ...)
    for (;;) {
        int k = again >= 0 ? again : read_key();
        bool replay = again >= 0;
        again = -1;
        if (k < 0) return done(!buf.empty());
        if (k == 27 && !replay) k = escape();
        bool was_tab = tabbed;
        tabbed = false;
        switch (k) {
            case '\r':
            case '\n': return done(true);
            case 4:  // Ctrl-D
                if (buf.empty()) {
                    out("\r\n");
                    tcsetattr(0, TCSADRAIN, &old);
                    return false;
                }
                [[fallthrough]];
            case K_DEL:
                if (pos < buf.size()) buf.erase(pos, next_char(buf, pos) - pos);
                break;
            case 3:  // Ctrl-C: drop the line
                out("^C");
                buf.clear();
                return done(true);
            case 127:
            case 8:
                if (pos > 0) {
                    size_t p = prev_char(buf, pos);
                    buf.erase(p, pos - p);
                    pos = p;
                }
                break;
            case 1:
            case K_HOME: pos = 0; break;
            case 5:
            case K_END: pos = buf.size(); break;
            case 2:
            case K_LEFT: pos = prev_char(buf, pos); break;
            case 6:
            case K_RIGHT: pos = next_char(buf, pos); break;
            case K_WLEFT:
                while (pos > 0 && buf[pos - 1] == ' ') --pos;
                while (pos > 0 && buf[pos - 1] != ' ') --pos;
                break;
            case K_WRIGHT:
                while (pos < buf.size() && buf[pos] == ' ') ++pos;
                while (pos < buf.size() && buf[pos] != ' ') ++pos;
                break;
            case 11: buf.erase(pos); break;  // Ctrl-K
            case 21:                         // Ctrl-U
                buf.erase(0, pos);
                pos = 0;
                break;
            case 23: {  // Ctrl-W
                size_t p = pos;
                while (p > 0 && buf[p - 1] == ' ') --p;
                while (p > 0 && buf[p - 1] != ' ') --p;
                buf.erase(p, pos - p);
                pos = p;
                break;
            }
            case 12: out("\x1b[H\x1b[2J"); break;  // Ctrl-L
            case 16:
            case K_UP:
                if (hidx > 0) {
                    if (hidx == history.size()) saved = buf;
                    buf = history[--hidx];
                    pos = buf.size();
                }
                break;
            case 14:
            case K_DOWN:
                if (hidx < history.size()) {
                    ++hidx;
                    buf = hidx == history.size() ? saved : history[hidx];
                    pos = buf.size();
                }
                break;
            case 18: {  // Ctrl-R: search back
                std::string q;
                size_t found = history.size();
                for (;;) {
                    std::string shown = found < history.size() ? history[found] : "";
                    out("\r(search) " + printable(q) + ": " + printable(shown) + "\x1b[K");
                    int c = read_key();
                    if (c == 127 || c == 8) {
                        if (!q.empty()) q.pop_back();
                    } else if (c == 18) {
                        size_t from = found;
                        found = history.size();
                        for (size_t h = from; h-- > 0;)
                            if (history[h].find(q) != std::string::npos) {
                                found = h;
                                break;
                            }
                        continue;
                    } else if (c >= 32 && c != 127) q += char(c);
                    else {
                        if (found < history.size() && c != 3 && c != 7) {
                            buf = history[found];
                            pos = buf.size();
                        }
                        // A key that ends the search is then done on the line found (bash does the
                        // same): an arrow or Ctrl-A moves in it, its bytes are never text (R2-9).
                        if (c == 27) c = escape();
                        if (c >= 0 && c != 3 && c != 7 && c != '\r' && c != '\n') again = c;
                        break;
                    }
                    found = history.size();
                    for (size_t h = history.size(); h-- > 0;)
                        if (history[h].find(q) != std::string::npos) {
                            found = h;
                            break;
                        }
                }
                break;
            }
            case '\t': {
                if (!complete) break;
                size_t start = word_start(buf, pos);
                auto c = complete(buf.substr(0, pos), start);
                if (c.empty()) break;
                std::string word = buf.substr(start, pos - start);
                std::string p = common_prefix(c);
                if (c.size() == 1) p += c[0].back() == '/' ? "" : " ";
                if (p.size() > word.size()) {
                    buf.replace(start, pos - start, p);
                    pos = start + p.size();
                } else if (was_tab || c.size() > 1) {
                    std::string list = "\r\n";
                    size_t n = 0;
                    for (auto& x : c) {
                        if (n++ >= 100) {
                            list += "...";
                            break;
                        }
                        list += printable(x) + "  ";
                    }
                    out(list + "\r\n");
                }
                tabbed = true;
                break;
            }
            default:
                if (k >= 32 && k < 256 && k != 127) {
                    buf.insert(pos, 1, char(k));
                    ++pos;
                }
        }
        // One draw per burst of input, not per byte: a 20,000-byte paste drew the whole line 20,000
        // times (200 MB to the terminal, audit R2-8/L-17).
        if (!pending()) draw();
    }
}

}  // namespace rsh
