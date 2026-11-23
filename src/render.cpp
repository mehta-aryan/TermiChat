// render.cpp – implementation of the pure rendering functions.

#include "render.hpp"
#include "line_editor.hpp"      // utf8_prev / utf8_next
#include "text_utils.hpp"

#include <wchar.h>              // mbrtowc, mbstate_t, wcwidth

#include <algorithm>
#include <iterator>

namespace {

constexpr const char* RESET = "\x1b[0m";

// Columns of the fancy layout:  " HH:MM  " is the lead before every line.
constexpr int TIME_COLS     = 5;
constexpr int LEAD_COLS     = 1 + TIME_COLS + 2;
constexpr int MIN_BODY_COLS = 10;   // narrower than this and wrapping makes no sense
constexpr int MIN_INPUT_COLS = 8;   // room the typed text needs before the prompt name is dropped

// ── Measuring ────────────────────────────────────────────────────────────────
struct Glyph { std::size_t len; int cols; };

// The character at s[i]: its bytes and its columns.  A byte that is not valid
// in the current locale counts as one column so measuring can never get stuck.
Glyph glyph_at(const std::string& s, std::size_t i) {
    std::mbstate_t state{};
    wchar_t wc = 0;
    const std::size_t n = std::mbrtowc(&wc, s.data() + i, s.size() - i, &state);
    if (n == static_cast<std::size_t>(-1) || n == static_cast<std::size_t>(-2)) return {1, 1};
    if (n == 0) return {1, 0};
    const int w = ::wcwidth(wc);
    return {n, w < 0 ? 1 : w};
}

// Length of the "ESC [ digits/; final" sequence at s[i], or 0 if there is none.
std::size_t escape_len(const std::string& s, std::size_t i) {
    if (s[i] != '\x1b' || i + 1 >= s.size() || s[i + 1] != '[') return 0;
    std::size_t j = i + 2;
    while (j < s.size() && s[j] >= 0x30 && s[j] <= 0x3F) ++j;
    if (j < s.size() && s[j] >= 0x40 && s[j] <= 0x7E) return j + 1 - i;
    return 0;
}

int width_between(const std::string& s, std::size_t from, std::size_t to) {
    return display_width(s.substr(from, to - from));
}

// ── Wrapping ─────────────────────────────────────────────────────────────────
struct Token {
    std::string text;
    int         cols  = 0;
    bool        space = false;   // a run of spaces; otherwise a word
};

// Splits into words and runs of spaces.  Colour sequences count as part of a
// word and as zero columns.
std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < s.size()) {
        Token t;
        t.space = (escape_len(s, i) == 0 && s[i] == ' ');
        while (i < s.size()) {
            const std::size_t esc = escape_len(s, i);
            if ((esc == 0 && s[i] == ' ') != t.space) break;
            if (esc != 0) { t.text.append(s, i, esc); i += esc; continue; }
            const Glyph g = glyph_at(s, i);
            t.text.append(s, i, g.len);
            t.cols += g.cols;
            i += g.len;
        }
        out.push_back(std::move(t));
    }
    return out;
}

// Collects wrapped lines.  'active' is the colour sequence in force at the end
// of the current line, so a break can close it and the next line can reopen it.
class Wrapper {
public:
    Wrapper(int first, int rest) : first_(first), rest_(rest) {}

    void place_word(const Token& word, const Token* space_before) {
        const int space_cols = space_before ? space_before->cols : 0;
        if (has_word_ && cols_ + space_cols + word.cols <= limit()) {
            if (space_before) append(space_before->text, space_cols);
            append(word.text, word.cols);
            return;
        }
        if (has_word_) break_line();                 // the space at the break is dropped
        if (word.cols <= limit()) append(word.text, word.cols);
        else hard_break(word.text);
    }

    std::vector<std::string> finish() {
        lines_.push_back(std::move(line_));
        return std::move(lines_);
    }

private:
    int first_, rest_;
    std::vector<std::string> lines_;
    std::string line_;
    int         cols_     = 0;
    bool        has_word_ = false;
    std::string active_;

    int limit() const { return lines_.empty() ? first_ : rest_; }

    void append(const std::string& piece, int piece_cols) {
        line_ += piece;
        cols_ += piece_cols;
        has_word_ = true;
        track_style(piece);
    }

    void track_style(const std::string& piece) {
        for (std::size_t i = 0; i < piece.size();) {
            const std::size_t esc = escape_len(piece, i);
            if (esc == 0) { ++i; continue; }
            const std::string seq = piece.substr(i, esc);
            if (seq == RESET || seq == "\x1b[m") active_.clear();
            else active_ = seq;
            i += esc;
        }
    }

    void break_line() {
        if (!active_.empty()) line_ += RESET;
        lines_.push_back(std::move(line_));
        line_ = active_;
        cols_ = 0;
        has_word_ = false;
    }

    // A word wider than a whole line: fill line after line, one character at a time.
    void hard_break(const std::string& word) {
        for (std::size_t i = 0; i < word.size();) {
            const std::size_t esc = escape_len(word, i);
            if (esc != 0) {                          // zero width: just keep it in order
                line_.append(word, i, esc);
                track_style(word.substr(i, esc));
                i += esc;
                continue;
            }
            const Glyph g = glyph_at(word, i);
            if (has_word_ && cols_ + g.cols > limit()) break_line();
            append(word.substr(i, g.len), g.cols);
            i += g.len;
        }
    }
};

// ── Styling helpers ──────────────────────────────────────────────────────────
std::string paint(const Style& st, const std::string& codes, const std::string& text) {
    if (!st.colour || codes.empty() || text.empty()) return text;
    return "\x1b[" + codes + "m" + text + RESET;
}

enum class Sym { Chevron, Join, Leave, Info, Online, Private, Arrow, Alert, Dot };

std::string sym(const Style& st, Sym s) {
    static const char* const UNI[] = {"❯", "→", "←", "•", "●", "✉", "→", "!", "·"};
    static const char* const ASC[] = {">", "->", "<-", "*", "*", "*", "->", "!", "-"};
    return (st.unicode ? UNI : ASC)[static_cast<int>(s)];
}

std::string spaces(int n) { return std::string(static_cast<std::size_t>(std::max(n, 0)), ' '); }

std::string repeat(const std::string& s, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

bool is_me(const std::string& name, const std::string& me) {
    return !me.empty() && to_lower(name) == to_lower(me);
}

// How a user is named in a line: "you" for this client, else the (shortened) name.
std::string shown_name(const std::string& name, const std::string& me) {
    return is_me(name, me) ? "you" : truncate_cols(name, MAX_NAME_COLS, "");
}

// " HH:MM  " in dim.
std::string time_field(const ChatEvent& ev, const Style& st) {
    return " " + paint(st, "2", format_clock(ev.timestamp)) + "  ";
}

// Puts 'head' (which takes 'head_cols' columns) before the wrapped 'body'.
// Later lines hang under the body, unless that would leave less than half the
// row for text: then they hang under the first word of the head instead.
std::vector<std::string> layout(const std::string& head, int head_cols,
                                const std::string& body, const Style& st) {
    if (st.width < MIN_WRAP_WIDTH || st.width - head_cols < MIN_BODY_COLS)
        return {head + body};
    const int indent = head_cols <= st.width / 2 ? head_cols : LEAD_COLS + 2;
    std::vector<std::string> lines = wrap_text(body, st.width - head_cols, st.width - indent);
    lines[0] = head + lines[0];
    for (std::size_t i = 1; i < lines.size(); ++i) lines[i] = spaces(indent) + lines[i];
    return lines;
}

// ── One renderer per event kind ──────────────────────────────────────────────
std::string plain_line(const ChatEvent& ev) {
    switch (ev.kind) {
    case EventKind::Message:
        return "[" + format_clock(ev.timestamp) + "] " + ev.sender + ": " + ev.text;
    case EventKind::Private:
        return "[" + format_clock(ev.timestamp) + "] [private] " + ev.sender + " -> "
             + ev.recipient + ": " + ev.text;
    case EventKind::System:
        return "*** " + ev.text;
    case EventKind::UserList:
        return "*** Online (" + std::to_string(ev.users.size()) + "): "
             + join_strings(ev.users, ", ");
    }
    return std::string();
}

std::vector<std::string> message_lines(const ChatEvent& ev, const std::string& me,
                                       const Style& st, RenderState& rs) {
    const bool mine = is_me(ev.sender, me);
    const std::string name = shown_name(ev.sender, me);
    const int name_cols = display_width(name);
    rs.name_cols = std::max(rs.name_cols, name_cols);

    const std::string chevron = sym(st, Sym::Chevron);
    const std::string codes = mine ? "1;32" : name_codes(ev.sender);
    const std::string head = time_field(ev, st) + paint(st, codes, name)
                           + spaces(rs.name_cols - name_cols) + " " + paint(st, "2", chevron) + " ";
    const int head_cols = LEAD_COLS + rs.name_cols + 1 + display_width(chevron) + 1;
    return layout(head, head_cols, ev.text, st);
}

std::vector<std::string> system_lines(const ChatEvent& ev, const Style& st) {
    Sym s = Sym::Info;
    const char* codes = "2;36";                      // info: dim cyan
    switch (ev.system_kind) {
    case SystemKind::Join:     s = Sym::Join;  codes = "2;32"; break;
    case SystemKind::Leave:    s = Sym::Leave; codes = "2;33"; break;
    case SystemKind::Error:    s = Sym::Alert; codes = "31";   break;
    case SystemKind::Shutdown: s = Sym::Alert; codes = "33";   break;
    case SystemKind::Info:     break;
    }
    const std::string symbol = sym(st, s);
    const std::string head = time_field(ev, st) + paint(st, codes, symbol) + " ";
    return layout(head, LEAD_COLS + display_width(symbol) + 1, paint(st, codes, ev.text), st);
}

std::vector<std::string> private_lines(const ChatEvent& ev, const std::string& me, const Style& st) {
    const std::string symbol = sym(st, Sym::Private);
    const std::string route = shown_name(ev.sender, me) + " " + sym(st, Sym::Arrow) + " "
                            + shown_name(ev.recipient, me) + ": ";
    const std::string head = time_field(ev, st) + paint(st, "35", symbol) + " " + paint(st, "35", route);
    const int head_cols = LEAD_COLS + display_width(symbol) + 1 + display_width(route);
    return layout(head, head_cols, paint(st, "35", ev.text), st);
}

std::vector<std::string> user_list_lines(const ChatEvent& ev, const std::string& me, const Style& st) {
    const std::string symbol = sym(st, Sym::Online);
    std::string body = paint(st, "2;36", "Online (" + std::to_string(ev.users.size()) + "): ");
    for (std::size_t i = 0; i < ev.users.size(); ++i) {
        if (i > 0) body += ", ";
        const std::string& u = ev.users[i];
        const std::string name = truncate_cols(u, MAX_NAME_COLS, "");
        if (is_me(u, me)) body += paint(st, "1;32", name) + paint(st, "2", " (you)");
        else              body += paint(st, name_codes(u), name);
    }
    const std::string head = time_field(ev, st) + paint(st, "2;36", symbol) + " ";
    return layout(head, LEAD_COLS + display_width(symbol) + 1, body, st);
}

struct Command { const char* name; const char* description; bool fancy_only; };
constexpr Command COMMANDS[] = {
    {"/help",               "show this help",                     false},
    {"/users",              "list who is online",                 false},
    {"/msg <user> <text>",  "send a private message to one user", false},
    {"/clear",              "clear the screen",                   true},
    {"/quit",               "leave the chat (Ctrl+D works too)",  false},
};
constexpr int COMMAND_COLS = 18;     // the widest command name, "/msg <user> <text>"

}  // namespace

// ── Public: measuring and wrapping ───────────────────────────────────────────
int display_width(const std::string& text) {
    int cols = 0;
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t esc = escape_len(text, i);
        if (esc != 0) { i += esc; continue; }
        const Glyph g = glyph_at(text, i);
        cols += g.cols;
        i += g.len;
    }
    return cols;
}

std::string truncate_cols(const std::string& text, int max_cols, const std::string& tail) {
    if (max_cols <= 0) return std::string();
    if (display_width(text) <= max_cols) return text;
    const int budget = std::max(0, max_cols - display_width(tail));
    std::size_t i = 0;
    int cols = 0;
    while (i < text.size()) {
        const Glyph g = glyph_at(text, i);
        if (cols + g.cols > budget) break;
        cols += g.cols;
        i += g.len;
    }
    return text.substr(0, i) + tail;
}

std::vector<std::string> wrap_text(const std::string& text, int first_width, int rest_width) {
    Wrapper wrapper(std::max(first_width, 1), std::max(rest_width, 1));
    const std::vector<Token> tokens = tokenize(text);
    const Token* space = nullptr;
    for (const Token& t : tokens) {
        if (t.space) { space = &t; continue; }
        wrapper.place_word(t, space);
        space = nullptr;
    }
    return wrapper.finish();
}

// ── Public: events ───────────────────────────────────────────────────────────
static std::string clean_name(const std::string& raw) {
    if (raw.empty()) return raw;
    const std::string s = sanitize_text(raw);
    return s.empty() ? "?" : s;            // a name must never vanish from the layout
}

void sanitize_event(ChatEvent& ev) {
    ev.sender    = clean_name(ev.sender);
    ev.recipient = clean_name(ev.recipient);
    ev.text      = sanitize_text(ev.text);
    for (std::string& u : ev.users) u = clean_name(u);
}

std::string name_codes(const std::string& name) {
    // Bright, readable on a dark background, and deliberately no red.
    static const int PALETTE[] = {75, 81, 111, 123, 141, 177, 213, 220, 215, 186};
    std::uint32_t h = 2166136261u;                       // FNV-1a
    for (unsigned char c : to_lower(name)) { h ^= c; h *= 16777619u; }
    return "1;38;5;" + std::to_string(PALETTE[h % std::size(PALETTE)]);
}

std::vector<std::string> render_event(const ChatEvent& ev, const std::string& me,
                                      const Style& st, RenderState& rs) {
    if (st.plain) return {plain_line(ev)};
    switch (ev.kind) {
    case EventKind::Message:  return message_lines(ev, me, st, rs);
    case EventKind::Private:  return private_lines(ev, me, st);
    case EventKind::System:   return system_lines(ev, st);
    case EventKind::UserList: return user_list_lines(ev, me, st);
    }
    return {};
}

std::vector<std::string> render_banner(const std::string& host, uint16_t port,
                                       const std::string& me, const Style& st) {
    const std::vector<std::string> content = {
        "TermiChat",
        "connected to " + sanitize_text(host) + ":" + std::to_string(port),
        "you are " + me + " " + sym(st, Sym::Dot) + " type /help for commands",
    };
    if (st.width < MIN_WRAP_WIDTH) {                      // too narrow for a box
        std::vector<std::string> lines = content;
        lines[0] = paint(st, "1", lines[0]);
        return lines;
    }
    int inner = 0;
    for (const std::string& c : content) inner = std::max(inner, display_width(c) + 4);
    inner = std::min(inner, st.width - 2);
    const int room = inner - 4;                           // two columns of padding each side

    const bool u = st.unicode;
    const std::string tl = u ? "╭" : "+", tr = u ? "╮" : "+", bl = u ? "╰" : "+", br = u ? "╯" : "+";
    const std::string h  = u ? "─" : "-", v  = u ? "│" : "|";

    std::vector<std::string> lines;
    lines.push_back(paint(st, "2", tl + repeat(h, inner) + tr));
    for (std::size_t i = 0; i < content.size(); ++i) {
        const std::string text = truncate_cols(content[i], room, u ? "…" : "...");
        lines.push_back(paint(st, "2", v) + "  " + paint(st, i == 0 ? "1" : "", text)
                        + spaces(room - display_width(text)) + "  " + paint(st, "2", v));
    }
    lines.push_back(paint(st, "2", bl + repeat(h, inner) + br));
    return lines;
}

std::vector<std::string> render_help(const Style& st, int64_t now) {
    std::vector<std::string> lines;
    if (st.plain) {
        lines.push_back("*** Commands:");
        for (const Command& c : COMMANDS)
            if (!c.fancy_only)
                lines.push_back("***   " + std::string(c.name)
                                + spaces(COMMAND_COLS - static_cast<int>(std::string(c.name).size()))
                                + "  " + c.description);
        return lines;
    }
    ChatEvent clock;
    clock.timestamp = now;
    const std::string symbol = sym(st, Sym::Info);
    lines.push_back(time_field(clock, st) + paint(st, "2;36", symbol + " Commands:"));
    const int indent = LEAD_COLS + 2;
    for (const Command& c : COMMANDS) {
        const std::string name = c.name;
        const std::string head = spaces(indent) + paint(st, "1", name)
                               + spaces(COMMAND_COLS - static_cast<int>(name.size())) + "  ";
        const std::vector<std::string> row = layout(head, indent + COMMAND_COLS + 2, c.description, st);
        lines.insert(lines.end(), row.begin(), row.end());
    }
    return lines;
}

// ── Public: input row ────────────────────────────────────────────────────────
// Moves 'pos' left by about 'cols' columns, onto a character boundary.
static std::size_t back_by_cols(const std::string& s, std::size_t pos, int cols) {
    int moved = 0;
    while (pos > 0 && moved < cols) {
        const std::size_t prev = utf8_prev(s, pos);
        moved += width_between(s, prev, pos);
        pos = prev;
    }
    return pos;
}

InputRow render_input_row(const std::string& username, const std::string& buffer,
                          std::size_t cursor, std::size_t& scroll, const Style& st) {
    const int avail = std::max(st.width - 1, 1);          // never touch the last column
    const std::string chevron = sym(st, Sym::Chevron);
    std::string prompt = paint(st, "1", username) + " " + paint(st, "2", chevron) + " ";
    int prompt_cols = display_width(username) + 1 + display_width(chevron) + 1;
    if (avail - prompt_cols < MIN_INPUT_COLS) {           // narrow: keep just the chevron
        prompt = paint(st, "2", chevron) + " ";
        prompt_cols = display_width(chevron) + 1;
    }
    const int text_cols = std::max(avail - prompt_cols, 1);

    cursor = std::min(cursor, buffer.size());
    scroll = std::min(scroll, buffer.size());
    if (scroll < buffer.size()) scroll = utf8_prev(buffer, utf8_next(buffer, scroll));  // snap to a boundary
    if (cursor < scroll) scroll = cursor;                 // the cursor left the window on the left
    if (cursor == scroll && scroll > 0)                   // sitting on the edge: show some context
        scroll = back_by_cols(buffer, scroll, text_cols / 2);
    while (width_between(buffer, scroll, cursor) > text_cols)   // ...or on the right
        scroll = utf8_next(buffer, scroll);

    std::size_t end = scroll;
    int cols = 0;
    while (end < buffer.size()) {
        const Glyph g = glyph_at(buffer, end);
        if (cols + g.cols > text_cols) break;
        cols += g.cols;
        end += g.len;
    }
    return {prompt + buffer.substr(scroll, end - scroll),
            prompt_cols + width_between(buffer, scroll, cursor)};
}
