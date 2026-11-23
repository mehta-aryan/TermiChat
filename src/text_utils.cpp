// text_utils.cpp – implementation of the shared text helpers.

#include "text_utils.hpp"

#include <ctime>

// ── Usernames ────────────────────────────────────────────────────────────────
std::string to_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

static bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

NameCheck check_username(const std::string& name) {
    if (name.empty() || name.size() > MAX_USERNAME_LEN) return NameCheck::BadLength;
    for (char c : name)
        if (!is_name_char(c)) return NameCheck::BadChars;
    const std::string lower = to_lower(name);
    if (lower == "server" || lower == "system") return NameCheck::Reserved;
    return NameCheck::Ok;
}

const char* name_problem(NameCheck check) {
    switch (check) {
    case NameCheck::Ok:        return "Username is valid";
    case NameCheck::BadLength: return "Username must be 1-16 characters long";
    case NameCheck::BadChars:  return "Username may only contain letters, digits, '_' and '-'";
    case NameCheck::Reserved:  return "That username is reserved";
    }
    return "Invalid username";
}

// ── sanitize_text ────────────────────────────────────────────────────────────
// UTF-8 is validated by hand, following the table in the Unicode standard:
//
//   lead byte   length  allowed 2nd byte   why
//   C2..DF      2       80..BF             (C0, C1 would be overlong)
//   E0          3       A0..BF             (80..9F would be overlong)
//   E1..EC,EE,EF 3      80..BF
//   ED          3       80..9F             (A0..BF would be a surrogate)
//   F0          4       90..BF             (80..8F would be overlong)
//   F1..F3      4       80..BF
//   F4          4       80..8F             (above would exceed U+10FFFF)
//
// Every further byte must be 80..BF.  Anything else is invalid.
std::string sanitize_text(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    const std::size_t n = text.size();
    std::size_t i = 0;

    while (i < n) {
        const auto b = static_cast<unsigned char>(text[i]);

        if (b < 0x80) {                         // ASCII
            if (b == '\t') out += ' ';
            else if (b >= 0x20 && b != 0x7F) out += static_cast<char>(b);
            ++i;
            continue;
        }

        std::size_t   len = 0;                  // total bytes of this character
        unsigned char lo = 0x80, hi = 0xBF;     // allowed range of the 2nd byte
        if      (b >= 0xC2 && b <= 0xDF) len = 2;
        else if (b == 0xE0)              { len = 3; lo = 0xA0; }
        else if (b == 0xED)              { len = 3; hi = 0x9F; }
        else if (b >= 0xE1 && b <= 0xEF) len = 3;
        else if (b == 0xF0)              { len = 4; lo = 0x90; }
        else if (b == 0xF4)              { len = 4; hi = 0x8F; }
        else if (b >= 0xF1 && b <= 0xF3) len = 4;
        else {                                  // stray 80..BF, C0, C1, F5..FF
            out += '?';
            ++i;
            continue;
        }

        // Count how many bytes of this character are really there and valid.
        std::size_t used = 1;
        while (used < len && i + used < n) {
            const auto c = static_cast<unsigned char>(text[i + used]);
            const unsigned char min = (used == 1) ? lo : 0x80;
            const unsigned char max = (used == 1) ? hi : 0xBF;
            if (c < min || c > max) break;
            ++used;
        }

        if (used < len) {                       // truncated or invalid: one '?'
            out += '?';
            i += used;                          // the bad byte is looked at again
            continue;
        }

        // Complete, valid character.  C1 controls (C2 80 .. C2 9F) are dropped.
        const bool is_c1 = (b == 0xC2 && static_cast<unsigned char>(text[i + 1]) <= 0x9F);
        if (!is_c1) out.append(text, i, len);
        i += len;
    }

    const std::size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos) return std::string();
    const std::size_t last = out.find_last_not_of(' ');
    return out.substr(first, last - first + 1);
}

// ── Small helpers ────────────────────────────────────────────────────────────
std::string join_strings(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += sep;
        out += parts[i];
    }
    return out;
}

int64_t unix_now() {
    return static_cast<int64_t>(std::time(nullptr));
}

std::string format_clock(int64_t unix_ts) {
    const std::time_t t = static_cast<std::time_t>(unix_ts);
    std::tm tm_buf{};
    // localtime_r is the thread-safe POSIX variant: it fills our own buffer
    // instead of returning a pointer to a shared static one.
    if (localtime_r(&t, &tm_buf) == nullptr) return "--:--";
    char buf[8];
    if (std::strftime(buf, sizeof(buf), "%H:%M", &tm_buf) == 0) return "--:--";
    return buf;
}
