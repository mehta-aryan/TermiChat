#pragma once
// terminal.hpp – everything that talks to the terminal device itself:
// capability detection, raw mode, window size, signals, key decoding and the
// one function every screen write goes through.  The chat logic never calls
// termios or ioctl directly, so it stays testable without a terminal.

#include <csignal>
#include <cstddef>
#include <string>
#include <vector>

#include <termios.h>

// ── Capabilities ─────────────────────────────────────────────────────────────
struct TermCaps {
    bool interactive = false;   // draw the full UI (raw mode, prompt, banner)
    bool colour      = false;   // emit ANSI colours
    bool utf8        = false;   // symbols and box drawing instead of ASCII
};

// The pure decision, kept apart from the lookups so it can be tested:
//   interactive = both stdin and stdout are terminals and TERM is not "dumb";
//   colour      = interactive and NO_COLOR is unset or empty (no-color.org);
//   utf8        = interactive and the locale's character set is UTF-8.
TermCaps classify_terminal(bool stdin_tty, bool stdout_tty, const char* term,
                           const char* no_color, bool utf8_locale);

// Looks at the real stdin/stdout/environment.  setlocale() is only called
// when the UI is interactive, so plain mode (pipes, tests) is unaffected by
// the user's locale.
TermCaps detect_terminal();

// ── Output ───────────────────────────────────────────────────────────────────
// Writes all of 'data', retrying after EINTR and partial writes and waiting
// when a non-blocking fd is full.  Returns false on a real error.
bool write_all(int fd, const std::string& data);

struct TermSize { int cols; int rows; };

// Size of the terminal on 'fd'; 80x24 when it cannot be determined.
TermSize terminal_size(int fd);

// ── Raw mode ─────────────────────────────────────────────────────────────────
// What "raw enough for a line editor" means: no line buffering, no echo, no
// Ctrl+V / Ctrl+S / Ctrl+Z handling by the driver, one byte per read().  ISIG
// stays on so Ctrl+C still raises SIGINT.  Pure, so it is unit-tested.
termios make_raw(termios t);

// RAII: the terminal is switched in the constructor and restored in the
// destructor, so every way out of a scope (return, exception) puts it back.
class RawMode {
public:
    explicit RawMode(int fd);               // throws std::runtime_error
    ~RawMode() { restore(); }
    void restore() noexcept;                // idempotent

    RawMode(const RawMode&)            = delete;
    RawMode& operator=(const RawMode&) = delete;

private:
    int     fd_;
    termios saved_{};
    bool    active_ = false;
};

// ── Signals ──────────────────────────────────────────────────────────────────
// While alive: SIGINT/SIGTERM/SIGHUP only set a flag and SIGWINCH sets another.
// The handlers are installed without SA_RESTART so a blocked poll() wakes up
// at once.  The destructor puts the previous handlers back.
class SignalGuard {
public:
    SignalGuard();                          // throws std::runtime_error
    ~SignalGuard();
    static bool stop_requested();           // SIGINT, SIGTERM or SIGHUP arrived
    static bool take_resize();              // true once per SIGWINCH

    SignalGuard(const SignalGuard&)            = delete;
    SignalGuard& operator=(const SignalGuard&) = delete;

private:
    struct sigaction old_int_{}, old_term_{}, old_hup_{}, old_winch_{};
};

// ── Keys ─────────────────────────────────────────────────────────────────────
enum class KeyType {
    Char,                                   // 'text' holds one UTF-8 character
    Enter, Backspace, Delete,
    Left, Right, Up, Down, Home, End,
    KillLine,                               // Ctrl+U
    DeleteWord,                             // Ctrl+W
    Redraw,                                 // Ctrl+L
    EndOfInput,                             // Ctrl+D
    Escape,                                 // a lone ESC
};

struct Key {
    KeyType     type;
    std::string text;                       // only for Char
};

// Turns the raw bytes read from a terminal into keys.  Escape sequences and
// multibyte characters may arrive split over several reads: the decoder keeps
// its state between feed() calls.
class KeyDecoder {
public:
    std::vector<Key> feed(const char* data, std::size_t n);

    // True while an ESC or half a sequence is waiting for its next byte.
    bool pending() const { return state_ != State::Ground; }

    // Call when no further byte arrived in time: a waiting ESC was the Escape
    // key itself, any other unfinished sequence is dropped.
    std::vector<Key> flush();

private:
    enum class State { Ground, Esc, Csi, Ss3, Utf8 };

    State       state_ = State::Ground;
    std::string seq_;                       // CSI parameter bytes or UTF-8 bytes so far
    std::size_t utf8_len_ = 0;              // bytes the current character needs

    void step(unsigned char b, std::vector<Key>& out);
    void ground(unsigned char b, std::vector<Key>& out);
    void finish_csi(unsigned char final_byte, std::vector<Key>& out);
};
