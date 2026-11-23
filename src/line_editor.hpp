#pragma once
// line_editor.hpp – the text being typed: a UTF-8 buffer, a cursor and a
// history of sent lines.  Pure data manipulation: no terminal, no I/O.
//
// The cursor is a BYTE offset that always sits on a character boundary, and
// every operation moves by whole code points, so a multibyte character can
// never be cut in half.

#include <cstddef>
#include <deque>
#include <string>

// Offset of the character boundary before / after 'pos' (clamped to the string).
std::size_t utf8_prev(const std::string& s, std::size_t pos);
std::size_t utf8_next(const std::string& s, std::size_t pos);

class LineEditor {
public:
    explicit LineEditor(std::size_t max_bytes, std::size_t max_history = 50);

    const std::string& text() const { return buf_; }
    std::size_t        cursor() const { return cursor_; }

    // Inserts ONE printable character (UTF-8) at the cursor.  Returns false and
    // changes nothing for a control character, a malformed character, or when
    // the line would grow past 'max_bytes'.
    bool insert(const std::string& ch);

    void backspace();          // delete the character before the cursor
    void erase_forward();      // delete the character under the cursor
    void move_left();
    void move_right();
    void move_home();
    void move_end();
    void kill_line();          // Ctrl+U: throw the whole line away
    void delete_word();        // Ctrl+W: the spaces and the word before the cursor

    // Up / Down.  The first Up remembers the unfinished line so Down can bring it back.
    void history_prev();
    void history_next();

    // Returns the finished line, empties the editor and remembers the line
    // (unless it is blank or repeats the previous one).
    std::string submit();

private:
    std::size_t            max_bytes_;
    std::size_t            max_history_;
    std::string            buf_;
    std::size_t            cursor_ = 0;
    std::deque<std::string> history_;
    std::size_t            hist_pos_ = 0;    // == history_.size() while not browsing
    std::string            draft_;           // the unfinished line while browsing

    void load(const std::string& line);
};
