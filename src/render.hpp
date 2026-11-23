#pragma once
// render.hpp – turns chat events into terminal lines.  Pure: strings in,
// strings out, no I/O and no clock, so every look of the UI is unit-tested.
//
// Two layouts share one entry point:
//   plain  – exactly the line formats the client printed before the UI existed
//            ("[HH:MM] alice: hi", "*** text"): used for pipes and tests;
//   fancy  – the styled layout (timestamp column, aligned names, symbols,
//            word wrapping with a hanging indent).

#include "protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct Style {
    bool plain   = true;    // old "[HH:MM] name: text" / "*** text" formats, nothing else
    bool colour  = false;   // ANSI colours (fancy layout only)
    bool unicode = false;   // ❯ → ● box drawing; otherwise > -> * + - |
    int  width   = 80;      // terminal columns
};

// Below this width wrapping is not attempted and lines are printed whole.
inline constexpr int MIN_WRAP_WIDTH = 30;
// Names are shown at most this wide (usernames are 16 characters at most).
inline constexpr int MAX_NAME_COLS = 16;

// What the renderer remembers between events.
struct RenderState {
    int name_cols = 3;      // longest name shown so far ("you" is always possible)
};

// ── Text measuring and wrapping ──────────────────────────────────────────────
// Terminal columns used by 'text': mbrtowc + wcwidth per character, with
// "ESC [ ... final" sequences counting as zero.  Needs setlocale() for non-ASCII.
int display_width(const std::string& text);

// Cuts 'text' to at most 'max_cols' columns; when something was cut, 'tail'
// (e.g. "…") takes the place of the last columns.
std::string truncate_cols(const std::string& text, int max_cols, const std::string& tail);

// Breaks 'text' into lines at spaces; the first line may use 'first_width'
// columns, the others 'rest_width'.  Spaces at a break (and at the edges) are
// dropped, a word longer than a line is cut mid-word, and colour sequences are
// closed at the end of a line and reopened on the next so a style survives a
// break.  Always returns at least one line.
std::vector<std::string> wrap_text(const std::string& text, int first_width, int rest_width);

// ── Events ───────────────────────────────────────────────────────────────────
// Makes every server-supplied string safe to print (no ESC or other controls,
// no invalid UTF-8).  Apply to events that came from the network, not to the
// client's own notices.
void sanitize_event(ChatEvent& ev);

// SGR parameters ("1;38;5;75") of a user's name colour: chosen by hashing the
// lower-cased name, so a name always looks the same.  Never red (red = errors).
std::string name_codes(const std::string& name);

// The terminal lines for one event, without line terminators.  'me' is this
// client's username (shown as "you" in the fancy layout).
std::vector<std::string> render_event(const ChatEvent& ev, const std::string& me,
                                      const Style& style, RenderState& state);

// The box shown after joining (fancy layout only).
std::vector<std::string> render_banner(const std::string& host, uint16_t port,
                                       const std::string& me, const Style& style);

// The /help text.  'now' is only used for the timestamp of the fancy header.
std::vector<std::string> render_help(const Style& style, int64_t now);

// ── Input row ────────────────────────────────────────────────────────────────
struct InputRow {
    std::string text;       // prompt + visible part of the buffer, styled
    int         cursor_col; // 0-based column the cursor belongs on
};

// The one-row prompt.  A line longer than the row scrolls sideways: 'scroll'
// (a byte offset into 'buffer', kept by the caller between calls) is the first
// visible character and is adjusted so the cursor is always on screen.
InputRow render_input_row(const std::string& username, const std::string& buffer,
                          std::size_t cursor, std::size_t& scroll, const Style& style);
