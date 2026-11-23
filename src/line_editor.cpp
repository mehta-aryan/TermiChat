// line_editor.cpp – implementation of the line editor.

#include "line_editor.hpp"

static bool is_continuation(unsigned char b) { return (b & 0xC0) == 0x80; }

std::size_t utf8_prev(const std::string& s, std::size_t pos) {
    if (pos > s.size()) pos = s.size();
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 && is_continuation(static_cast<unsigned char>(s[pos]))) --pos;
    return pos;
}

std::size_t utf8_next(const std::string& s, std::size_t pos) {
    if (pos >= s.size()) return s.size();
    ++pos;
    while (pos < s.size() && is_continuation(static_cast<unsigned char>(s[pos]))) ++pos;
    return pos;
}

// Bytes a character with this lead byte must have (0 = not a valid lead byte).
static std::size_t expected_length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if (lead >= 0xC2 && lead <= 0xDF) return 2;
    if (lead >= 0xE0 && lead <= 0xEF) return 3;
    if (lead >= 0xF0 && lead <= 0xF4) return 4;
    return 0;
}

LineEditor::LineEditor(std::size_t max_bytes, std::size_t max_history)
    : max_bytes_(max_bytes), max_history_(max_history) {}

bool LineEditor::insert(const std::string& ch) {
    if (ch.empty()) return false;
    const auto lead = static_cast<unsigned char>(ch[0]);
    if (lead < 0x20 || lead == 0x7F) return false;               // control character
    if (expected_length(lead) != ch.size()) return false;        // not exactly one character
    for (std::size_t i = 1; i < ch.size(); ++i)
        if (!is_continuation(static_cast<unsigned char>(ch[i]))) return false;
    if (buf_.size() + ch.size() > max_bytes_) return false;      // line is full: ignore the key
    buf_.insert(cursor_, ch);
    cursor_ += ch.size();
    return true;
}

void LineEditor::backspace() {
    const std::size_t start = utf8_prev(buf_, cursor_);
    buf_.erase(start, cursor_ - start);
    cursor_ = start;
}

void LineEditor::erase_forward() {
    buf_.erase(cursor_, utf8_next(buf_, cursor_) - cursor_);
}

void LineEditor::move_left()  { cursor_ = utf8_prev(buf_, cursor_); }
void LineEditor::move_right() { cursor_ = utf8_next(buf_, cursor_); }
void LineEditor::move_home()  { cursor_ = 0; }
void LineEditor::move_end()   { cursor_ = buf_.size(); }

void LineEditor::kill_line() {
    buf_.clear();
    cursor_ = 0;
}

void LineEditor::delete_word() {
    // ' ' is a single byte that never occurs inside a multibyte character, so
    // stopping on it always leaves 'start' on a character boundary.
    std::size_t start = cursor_;
    while (start > 0 && buf_[start - 1] == ' ') --start;
    while (start > 0 && buf_[start - 1] != ' ') --start;
    buf_.erase(start, cursor_ - start);
    cursor_ = start;
}

void LineEditor::load(const std::string& line) {
    buf_    = line;
    cursor_ = buf_.size();
}

void LineEditor::history_prev() {
    if (hist_pos_ == 0) return;                      // already at the oldest (or no history)
    if (hist_pos_ == history_.size()) draft_ = buf_;
    --hist_pos_;
    load(history_[hist_pos_]);
}

void LineEditor::history_next() {
    if (hist_pos_ >= history_.size()) return;        // not browsing
    ++hist_pos_;
    load(hist_pos_ == history_.size() ? draft_ : history_[hist_pos_]);
}

std::string LineEditor::submit() {
    std::string line = std::move(buf_);
    buf_.clear();
    cursor_ = 0;
    draft_.clear();
    const bool blank = line.find_first_not_of(' ') == std::string::npos;
    if (!blank && (history_.empty() || history_.back() != line)) {
        history_.push_back(line);
        if (history_.size() > max_history_) history_.pop_front();
    }
    hist_pos_ = history_.size();
    return line;
}
