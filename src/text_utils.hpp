#pragma once
// text_utils.hpp – username rules, text sanitising and small helpers that the
// server and the client share.  Nothing here touches a socket.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

inline constexpr std::size_t MAX_USERNAME_LEN = 16;

// ── Usernames ────────────────────────────────────────────────────────────────
// A username is 1-16 characters from [A-Za-z0-9_-] and must not be one of the
// reserved names "server" / "system" (compared case-insensitively).  Because
// the charset is so small, a validated name is always safe to print in a log
// line or a terminal.
enum class NameCheck { Ok, BadLength, BadChars, Reserved };

NameCheck check_username(const std::string& name);

// Human-readable explanation of a failed check (sent to the user in JOIN_ERR).
const char* name_problem(NameCheck check);

// ASCII-only lower-casing (the locale must not change how names compare).
std::string to_lower(std::string s);

// ── Chat text ────────────────────────────────────────────────────────────────
// Makes untrusted text safe to show in a terminal:
//   - a tab becomes one space;
//   - every other byte below 0x20, and 0x7F, is dropped (so ESC sequences die);
//   - UTF-8-encoded C1 controls U+0080..U+009F (bytes C2 80..C2 9F) are dropped;
//   - invalid UTF-8 (stray continuation, overlong, surrogate, > U+10FFFF,
//     truncated) is replaced by '?', one per maximal invalid subpart;
//   - leading and trailing spaces are trimmed.
// The result may be empty, in which case the caller ignores the message.
std::string sanitize_text(const std::string& text);

// ── Small helpers ────────────────────────────────────────────────────────────
std::string join_strings(const std::vector<std::string>& parts, const std::string& sep);

// Seconds since the Unix epoch.
int64_t unix_now();

// "HH:MM" of a Unix timestamp in the local time zone (thread-safe, localtime_r).
// Returns "--:--" if the timestamp cannot be converted.
std::string format_clock(int64_t unix_ts);
