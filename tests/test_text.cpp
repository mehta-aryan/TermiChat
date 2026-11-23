// tests/test_text.cpp – unit tests for username rules, sanitize_text and the
// time helper.  Expected values are written out by hand (byte by byte for
// UTF-8), so a bug in the implementation cannot hide behind itself.

#include "check.hpp"
#include "../src/text_utils.hpp"

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>

// Shorthand: a std::string that may contain NUL bytes.
template <std::size_t N>
static std::string bytes(const char (&lit)[N]) { return std::string(lit, N - 1); }

static void test_username() {
    std::puts("Test 1: username validation");
    check(check_username("a") == NameCheck::Ok, "1 character is fine");
    check(check_username("Alice_01-x") == NameCheck::Ok, "letters, digits, '_' and '-' are fine");
    check(check_username(std::string(16, 'x')) == NameCheck::Ok, "16 characters is fine");
    check(check_username("servers") == NameCheck::Ok && check_username("server1") == NameCheck::Ok,
          "names that merely contain 'server' are fine");

    check(check_username("") == NameCheck::BadLength, "empty -> BadLength");
    check(check_username(std::string(17, 'x')) == NameCheck::BadLength, "17 characters -> BadLength");
    check(check_username("bad name") == NameCheck::BadChars, "space -> BadChars");
    check(check_username("a.b") == NameCheck::BadChars, "'.' -> BadChars");
    check(check_username("a|b") == NameCheck::BadChars, "'|' -> BadChars (it is the field separator)");
    check(check_username("a,b") == NameCheck::BadChars, "',' -> BadChars (it is the list separator)");
    check(check_username("caf\xc3\xa9") == NameCheck::BadChars, "non-ASCII -> BadChars");
    check(check_username(bytes("a\0b")) == NameCheck::BadChars, "NUL -> BadChars");
    check(check_username("a\nb") == NameCheck::BadChars, "newline -> BadChars");
    check(check_username("a\x1b[0m") == NameCheck::BadChars, "ESC -> BadChars");

    check(check_username("server") == NameCheck::Reserved, "'server' reserved");
    check(check_username("SERVER") == NameCheck::Reserved, "'SERVER' reserved (case-insensitive)");
    check(check_username("System") == NameCheck::Reserved, "'System' reserved");
    check(check_username("sYsTeM") == NameCheck::Reserved, "'sYsTeM' reserved");

    check(std::string(name_problem(NameCheck::BadLength)).find("1-16") != std::string::npos,
          "BadLength explanation mentions 1-16");
    check(std::string(name_problem(NameCheck::Reserved)).find("reserved") != std::string::npos,
          "Reserved explanation says so");
}

static void test_to_lower() {
    std::puts("Test 2: to_lower is ASCII-only");
    check(to_lower("MiXeD_09-") == "mixed_09-", "letters are lowered, the rest is untouched");
    check(to_lower("\xc3\x89") == "\xc3\x89", "non-ASCII bytes are left alone");
    check(to_lower("") == "", "empty stays empty");
}

static void test_sanitize_controls() {
    std::puts("Test 3: sanitize_text - control characters");
    check(sanitize_text("hello world") == "hello world", "plain text unchanged");
    check(sanitize_text("a\x1b[31mred\x1b[0m") == "a[31mred[0m", "ESC bytes are removed (sequence is defused)");
    check(sanitize_text("a\tb") == "a b", "tab -> one space");
    check(sanitize_text("\tx\t") == "x", "tabs at the edges become spaces and are trimmed");
    check(sanitize_text(bytes("a\0b\x07\x7f\r\nc\x01")) == "abc", "NUL, BEL, DEL, CR, LF, SOH are dropped");
    check(sanitize_text("\x1b\x07") == "", "only control bytes -> empty");
    check(sanitize_text("x\x1f" "y") == "xy", "0x1F (last control below the space) dropped");
    check(sanitize_text("x y") == "x y", "0x20 (a normal space) survives");
}

static void test_sanitize_c1() {
    std::puts("Test 4: sanitize_text - C1 controls (UTF-8 encoded)");
    check(sanitize_text("a\xc2\x9b" "31m") == "a31m", "U+009B (CSI) dropped");
    check(sanitize_text("\xc2\x80" "x" "\xc2\x9f") == "x", "U+0080 and U+009F dropped (range ends)");
    check(sanitize_text("\xc2\xa0") == "\xc2\xa0", "U+00A0 (just past the range) kept");
    check(sanitize_text("\xc2\x7f") == "?", "C2 followed by ASCII is invalid (one ?, the DEL is dropped), not a C1");
}

static void test_sanitize_utf8() {
    std::puts("Test 5: sanitize_text - UTF-8");
    check(sanitize_text("caf\xc3\xa9") == "caf\xc3\xa9", "2-byte character kept");
    check(sanitize_text("\xe2\x82\xac") == "\xe2\x82\xac", "3-byte character (euro) kept");
    check(sanitize_text("\xf0\x9f\x98\x80") == "\xf0\x9f\x98\x80", "4-byte character (emoji) kept");
    check(sanitize_text("\xed\x9f\xbf") == "\xed\x9f\xbf", "U+D7FF (last before the surrogates) kept");
    check(sanitize_text("\xf4\x8f\xbf\xbf") == "\xf4\x8f\xbf\xbf", "U+10FFFF (the maximum) kept");
    check(sanitize_text("\xef\xbf\xbd") == "\xef\xbf\xbd", "U+FFFD kept");

    check(sanitize_text("\xff") == "?", "0xFF -> ?");
    check(sanitize_text("a\x80" "b") == "a?b", "stray continuation byte -> ?");
    check(sanitize_text("\xc0\xaf") == "??", "overlong '/' (C0 AF) -> two ?");
    check(sanitize_text("\xc1\xbf") == "??", "overlong (C1 BF) -> two ?");
    check(sanitize_text("\xe0\x80\x80") == "???", "overlong 3-byte (E0 80 80) -> ???");
    check(sanitize_text("\xf0\x80\x80\x80") == "????", "overlong 4-byte (F0 80 80 80) -> ????");
    check(sanitize_text("\xed\xa0\x80") == "???", "surrogate U+D800 (ED A0 80) -> ???");
    check(sanitize_text("\xed\xbf\xbf") == "???", "surrogate U+DFFF (ED BF BF) -> ???");
    check(sanitize_text("\xf4\x90\x80\x80") == "????", "U+110000 (F4 90 80 80) -> ????");
    check(sanitize_text("\xf5\x80\x80\x80") == "????", "F5 lead byte -> ????");
    check(sanitize_text("caf\xc3") == "caf?", "2-byte character cut off at the end -> one ?");
    check(sanitize_text("\xe2\x82" "x") == "?x", "3-byte character cut after 2 bytes -> one ?, next byte kept");
    check(sanitize_text("\xf0\x9f\x98") == "?", "4-byte character cut after 3 bytes -> one ?");
    check(sanitize_text("\xc3" "A") == "?A", "lead byte followed by ASCII -> ? then the ASCII");
}

static void test_sanitize_trim() {
    std::puts("Test 6: sanitize_text - trimming");
    check(sanitize_text("  hi  ") == "hi", "leading and trailing spaces trimmed");
    check(sanitize_text("   ") == "", "only spaces -> empty");
    check(sanitize_text("") == "", "empty -> empty");
    check(sanitize_text(" a  b ") == "a  b", "inner spaces untouched");
    check(sanitize_text("\x1b x \x07") == "x", "spaces exposed by dropping controls are trimmed too");
}

// Independent strict UTF-8 checker for the property test below.
static bool valid_utf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto b = static_cast<unsigned char>(s[i]);
        std::size_t len; uint32_t cp, min;
        if (b < 0x80)                { ++i; continue; }
        else if ((b & 0xE0) == 0xC0) { len = 2; cp = b & 0x1F; min = 0x80; }
        else if ((b & 0xF0) == 0xE0) { len = 3; cp = b & 0x0F; min = 0x800; }
        else if ((b & 0xF8) == 0xF0) { len = 4; cp = b & 0x07; min = 0x10000; }
        else return false;
        if (i + len > s.size()) return false;
        for (std::size_t k = 1; k < len; ++k) {
            const auto c = static_cast<unsigned char>(s[i + k]);
            if ((c & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

static void test_sanitize_property() {
    std::puts("Test 7: sanitize_text - output is always terminal-safe (20000 pseudo-random inputs)");
    uint32_t state = 12345;
    auto next = [&state] { state = state * 1664525u + 1013904223u; return state >> 24; };

    bool safe = true, utf8 = true, trimmed = true, no_c1 = true;
    for (int round = 0; round < 20000; ++round) {
        std::string in;
        const std::size_t len = next() % 24;
        for (std::size_t k = 0; k < len; ++k) {
            // Bias towards bytes that matter: controls, ESC, C2, continuation, lead bytes.
            const uint32_t r = next();
            in += static_cast<char>(r % 4 == 0 ? (r >> 2) % 0x20
                                    : r % 4 == 1 ? 0x80 + (r >> 2) % 0x80
                                                 : r);
        }
        const std::string out = sanitize_text(in);
        for (char c : out) {
            const auto u = static_cast<unsigned char>(c);
            if (u < 0x20 || u == 0x7F) safe = false;
        }
        if (!valid_utf8(out)) utf8 = false;
        if (!out.empty() && (out.front() == ' ' || out.back() == ' ')) trimmed = false;
        for (unsigned char lo = 0x80; lo <= 0x9F; ++lo)
            if (out.find(std::string("\xc2") + static_cast<char>(lo)) != std::string::npos) no_c1 = false;
        if (out.size() > in.size()) safe = false;       // sanitising never grows the text
    }
    check(safe,    "no byte < 0x20 or 0x7F survives, and the text never grows");
    check(utf8,    "the result is always valid UTF-8");
    check(trimmed, "the result never starts or ends with a space");
    check(no_c1,   "no C1 control (C2 80..C2 9F) survives");
}

static void test_helpers() {
    std::puts("Test 8: join_strings and the clock");
    check(join_strings({"a", "b", "c"}, ", ") == "a, b, c", "join with ', '");
    check(join_strings({"solo"}, ", ") == "solo", "one element, no separator");
    check(join_strings({}, ", ").empty(), "no elements -> empty string");

    // POSIX TZ strings need no tzdata: "UTC0" = UTC, "IST-5:30" = UTC+05:30.
    setenv("TZ", "UTC0", 1); tzset();
    check(format_clock(0) == "00:00", "timestamp 0 in UTC is 00:00");
    check(format_clock(86399) == "23:59", "86399 in UTC is 23:59");
    check(format_clock(3661) == "01:01", "3661 in UTC is 01:01");
    setenv("TZ", "IST-5:30", 1); tzset();
    check(format_clock(0) == "05:30", "timestamp 0 in UTC+05:30 is 05:30 (local zone is used)");
    check(format_clock(INT64_C(9000000000000000000)) == "--:--", "an unconvertible timestamp gives --:--");
    check(unix_now() > 1700000000, "unix_now() is a plausible current time");
}

int main() {
    std::puts("=== test_text ===");
    test_username();
    test_to_lower();
    test_sanitize_controls();
    test_sanitize_c1();
    test_sanitize_utf8();
    test_sanitize_trim();
    test_sanitize_property();
    test_helpers();
    return summary();
}
