// SPDX-License-Identifier: GPL-2.0-or-later
// rsh unit tests for the Regexp fast path (src/rt.cpp, audit H-1): utf8_kind must call a subject
// valid only when the engine itself does, because a valid subject is matched with
// PCRE2_NO_UTF_CHECK (undefined behaviour on invalid UTF-8). Checked against the engine on fixed
// cases and on a million random byte strings biased towards UTF-8 lead and continuation bytes.
// Then utf8_bad against the engine's own error offsets, and frag_match (an invalid subject
// matched from its valid pieces) against the engine with PCRE2_MATCH_INVALID_UTF: the same result
// and offsets for 41 patterns (each also caseless), random subjects with invalid bytes, and every start offset.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <pcre2.h>

#include "rt.hpp"

static int failures = 0, checks = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        ++checks;                                                                  \
        if (!(cond)) {                                                             \
            ++failures;                                                            \
            std::fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)

static pcre2_code* code;
static pcre2_match_data* md;

// The engine's verdict: 1 valid UTF-8, -1 not.
static int engine(const std::string& s) {
    int rc = pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(s.data()), s.size(), 0, 0, md, nullptr);
    return rc <= PCRE2_ERROR_UTF8_ERR1 && rc >= PCRE2_ERROR_UTF8_ERR21 ? -1 : 1;
}

static bool ascii(const std::string& s) {
    for (unsigned char c : s)
        if (c >= 0x80) return false;
    return true;
}

// Appends code point c (a surrogate too: the engine must reject it, and so must utf8_kind).
static void utf8(std::string& o, uint32_t c) {
    if (c < 0x80) o += char(c);
    else if (c < 0x800) o += char(0xC0 | (c >> 6)), o += char(0x80 | (c & 0x3F));
    else if (c < 0x10000) o += char(0xE0 | (c >> 12)), o += char(0x80 | ((c >> 6) & 0x3F)), o += char(0x80 | (c & 0x3F));
    else {
        o += char(0xF0 | (c >> 18));
        o += char(0x80 | ((c >> 12) & 0x3F));
        o += char(0x80 | ((c >> 6) & 0x3F));
        o += char(0x80 | (c & 0x3F));
    }
}

static int seen[3];  // invalid, valid with non-ASCII, ASCII

static void same(const std::string& s) {
    int k = rsh::utf8_kind(s.data(), s.size());
    ++seen[k < 0 ? 0 : k];
    int e = engine(s);
    bool ok = (k > 0) == (e > 0) && (k == 2) == (e > 0 && ascii(s));
    if (!ok) {
        std::fprintf(stderr, "utf8_kind %d, engine %d for:", k, e);
        for (unsigned char c : s) std::fprintf(stderr, " %02x", c);
        std::fprintf(stderr, "\n");
    }
    CHECK(ok);
}

int main() {
    int ec;
    PCRE2_SIZE eo;
    code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>("x"), 1, PCRE2_UTF, &ec, &eo, nullptr);
    md = pcre2_match_data_create_from_pattern(code, nullptr);
    CHECK(code && md);

    const char* fixed[] = {"", "plain ascii text, longer than eight bytes", "caf\xC3\xA9", "caf\xC3",
                           "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "a\0b", "\xC0\xAF", "\xC1\xBF", "\xC2\x80",
                           "\xE0\x80\xAF", "\xE0\xA0\x80", "\xED\x9F\xBF", "\xED\xA0\x80", "\xEF\xBF\xBF",
                           "\xF0\x8F\xBF\xBF", "\xF0\x90\x80\x80", "\xF4\x8F\xBF\xBF", "\xF4\x90\x80\x80",
                           "\xF5\x80\x80\x80", "\xF8\x88\x80\x80\x80", "\xFF", "\x80", "abcdefg\xC3", "abcdefgh\xC3\xA9"};
    for (const char* f : fixed) same(std::string(f, f == fixed[5] ? 3 : std::strlen(f)));
    CHECK(rsh::utf8_kind("abc", 3) == 2);
    CHECK(rsh::utf8_kind("caf\xC3\xA9", 5) == 1);
    CHECK(rsh::utf8_kind("caf\xC3", 4) == -1);

    // Random strings: a fixed generator, bytes drawn mostly from the interesting ranges.
    uint32_t x = 1;
    auto next = [&] { return x = x * 1103515245u + 12345u, x >> 8; };
    const unsigned char pool[] = {'a', 0, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF, 0xC0, 0xC1, 0xC2, 0xDF, 0xE0,
                                  0xE1, 0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF3, 0xF4, 0xF5, 0xFE, 0xFF};
    for (int n = 0; n < 1000000; ++n) {
        std::string s;
        int len = int(next() % 8);
        for (int i = 0; i < len; ++i) {
            uint32_t r = next() % 6;
            if (r == 0) s += char('a' + next() % 26);
            else if (r <= 2) s += char(pool[next() % sizeof pool]);
            else utf8(s, r == 3 ? 0x80 + next() % 0x780 : r == 4 ? 0x800 + next() % 0xF800 : 0x10000 + next() % 0x100000);
        }
        same(s);
        if (failures > 20) break;
    }
    std::printf("regexp: random strings: %d invalid, %d valid UTF-8, %d ASCII\n", seen[0], seen[1], seen[2]);
    CHECK(seen[0] > 100000 && seen[1] > 10000 && seen[2] > 10000);  // the random strings reach all three
    // utf8_bad: the offsets the engine reports, one after the other (its restart rule).
    x = 7;
    for (int n = 0; n < 200000; ++n) {
        std::string t;
        int len = int(next() % 10);
        for (int i = 0; i < len; ++i) {
            uint32_t r = next() % 4;
            if (r == 0) t += char('a' + next() % 3);
            else if (r == 1) t += char(pool[next() % sizeof pool]);
            else utf8(t, r == 2 ? 0x80 + next() % 0x780 : 0x800 + next() % 0xF800);
        }
        std::vector<size_t> want;
        for (size_t at = 0; at < t.size();) {
            int rc = pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(t.data() + at), t.size() - at, 0, 0, md, nullptr);
            if (!(rc <= PCRE2_ERROR_UTF8_ERR1 && rc >= PCRE2_ERROR_UTF8_ERR21)) break;
            size_t b = at + pcre2_get_startchar(md);
            want.push_back(b);
            at = b + 1;
            while (at < t.size() && (static_cast<unsigned char>(t[at]) & 0xC0) == 0x80) ++at;
        }
        size_t got[16];
        size_t k = rsh::utf8_bad(t.data(), t.size(), got, 16);
        bool ok = k == want.size() && std::equal(want.begin(), want.end(), got);
        if (!ok) {
            std::fprintf(stderr, "utf8_bad differs for:");
            for (unsigned char c : t) std::fprintf(stderr, " %02x", c);
            std::fprintf(stderr, "\n");
        }
        CHECK(ok);
        if (failures > 20) break;
    }

    // frag_match against the engine's own invalid-UTF matching (patterns with \\A, \\G, \\z or \\Z do
    // not go through it: a piece passed as the subject moves where they match, and PCRE2 10.45 and
    // later match \\z and \\Z at the end of the whole subject only; Re::frag in src/rt.cpp).
    const char* pats[] = {".", "\\w+", "\\b\\w", "^", "$", "^\\w", "\\w$", "", "a|", "(?<=a)b",
                          "(?<=ab)c", "(?<!a)b", "(?=b)", "\\B", "[^a]", "\\X", "(a)?(b)", "x*", "\\s", "\xC3\xA9",
                          "\xE6\x97\xA5", "^a$", "a.b", "(?s).", "\\p{L}+", "(?<=\\w{2})x", "b\\b",
                          "\\n", "^$", "(?i)\xC3\x89", "[\\x{80}-\\x{10FFFF}]", ".{2}", "(?<=^)a", "(?<=\\n)\\w",
                          "\\w(?=\\W)", "(?<=.)$", "a(?<!^a)", "(?<=\xC3\xA9)a", "\\d|\\W"};
    const char* toks[] = {"a", "b", "x", " ", "\n", "\xC3\xA9", "\xE6\x97\xA5", "\xF0\x9F\x98\x80", "\xFF", "\x80", "\xC3",
                          "\xE6\x97", "\xED\xA0\x80", "\xC0\xAF"};
    pcre2_match_data* m1 = pcre2_match_data_create(10, nullptr);
    pcre2_match_data* m2 = pcre2_match_data_create(10, nullptr);
    long compared = 0, matched = 0;
    for (const char* pat : pats) {
        for (uint32_t ci = 0; ci < 2; ++ci) {
            uint32_t o = PCRE2_UTF | PCRE2_MULTILINE | (ci ? PCRE2_CASELESS : 0);
            pcre2_code* fast = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pat), PCRE2_ZERO_TERMINATED, o, &ec, &eo, nullptr);
            pcre2_code* slow = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pat), PCRE2_ZERO_TERMINATED,
                                             o | PCRE2_MATCH_INVALID_UTF, &ec, &eo, nullptr);
            CHECK(fast && slow);
            if (!fast || !slow) continue;
            uint32_t lb = 0;
            pcre2_pattern_info(fast, PCRE2_INFO_MAXLOOKBEHIND, &lb);
            x = 99;
            for (int n = 0; n < 1500; ++n) {
                std::string t;
                int len = int(next() % 9);
                for (int i = 0; i < len; ++i) t += toks[next() % (sizeof toks / sizeof *toks)];
                size_t bad[64];
                size_t nb = rsh::utf8_bad(t.data(), t.size(), bad, 64);
                for (size_t st = 0; st <= t.size(); ++st) {
                    int r1 = rsh::frag_match(fast, t.data(), t.size(), st, bad, nb, lb, m1, nullptr);
                    int r2 = pcre2_match(slow, reinterpret_cast<PCRE2_SPTR>(t.data()), t.size(), st, 0, m2, nullptr);
                    bool ok = r1 == r2;
                    PCRE2_SIZE *v1 = pcre2_get_ovector_pointer(m1), *v2 = pcre2_get_ovector_pointer(m2);
                    for (int g = 0; ok && g < 2 * r1; ++g) ok = v1[g] == v2[g];
                    ++compared;
                    matched += r2 > 0;
                    if (!ok) {
                        std::fprintf(stderr, "frag_match differs: /%s/%s at %zu: %d [%zu,%zu] vs %d [%zu,%zu] for:", pat,
                                     ci ? "i" : "", st, r1, r1 > 0 ? v1[0] : 0, r1 > 0 ? v1[1] : 0, r2,
                                     r2 > 0 ? v2[0] : 0, r2 > 0 ? v2[1] : 0);
                        for (unsigned char c : t) std::fprintf(stderr, " %02x", c);
                        std::fprintf(stderr, "\n");
                    }
                    CHECK(ok);
                    if (failures > 40) break;
                }
                if (failures > 40) break;
            }
            pcre2_code_free(fast);
            pcre2_code_free(slow);
        }
    }
    std::printf("regexp: frag_match: %ld matches compared, %ld found\n", compared, matched);
    pcre2_match_data_free(m1);
    pcre2_match_data_free(m2);
    pcre2_match_data_free(md);
    pcre2_code_free(code);
    std::printf("regexp: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
