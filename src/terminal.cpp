// terminal.cpp – implementation of the terminal helpers.

#include "terminal.hpp"
#include "text_utils.hpp"

#include <langinfo.h>      // nl_langinfo
#include <poll.h>
#include <sys/ioctl.h>     // TIOCGWINSZ
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <clocale>         // setlocale
#include <cstdlib>
#include <cstring>
#include <stdexcept>

// ── Capabilities ─────────────────────────────────────────────────────────────
TermCaps classify_terminal(bool stdin_tty, bool stdout_tty, const char* term,
                           const char* no_color, bool utf8_locale) {
    TermCaps caps;
    const bool dumb = (term == nullptr || std::strcmp(term, "dumb") == 0);
    caps.interactive = stdin_tty && stdout_tty && !dumb;
    caps.colour      = caps.interactive && !(no_color != nullptr && no_color[0] != '\0');
    caps.utf8        = caps.interactive && utf8_locale;
    return caps;
}

static bool locale_is_utf8() {
    // The UI decides symbols from the codeset, so the user's locale must be
    // loaded first; the default "C" locale would always answer ANSI_X3.4-1968.
    if (std::setlocale(LC_ALL, "") == nullptr) return false;
    const std::string codeset = to_lower(nl_langinfo(CODESET));
    return codeset == "utf-8" || codeset == "utf8";
}

TermCaps detect_terminal() {
    const bool in_tty  = ::isatty(STDIN_FILENO) != 0;
    const bool out_tty = ::isatty(STDOUT_FILENO) != 0;
    const char* term   = std::getenv("TERM");
    const char* no_col = std::getenv("NO_COLOR");
    // Decide interactivity first so a pipe never pays for (or is changed by) setlocale().
    const bool interactive = classify_terminal(in_tty, out_tty, term, no_col, false).interactive;
    return classify_terminal(in_tty, out_tty, term, no_col,
                             interactive && locale_is_utf8());
}

// ── Output ───────────────────────────────────────────────────────────────────
static void wait_writable(int fd) {
    pollfd pfd{};
    pfd.fd     = fd;
    pfd.events = POLLOUT;
    ::poll(&pfd, 1, 100);    // a timeout or EINTR just means "try the write again"
}

bool write_all(int fd, const std::string& data) {
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { wait_writable(fd); continue; }
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

TermSize terminal_size(int fd) {
    winsize ws{};
    if (::ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return {ws.ws_col, ws.ws_row > 0 ? ws.ws_row : 24};
    return {80, 24};
}

// ── Raw mode ─────────────────────────────────────────────────────────────────
termios make_raw(termios t) {
    t.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | IEXTEN);   // IEXTEN: else Ctrl+V is eaten
    t.c_lflag |= ISIG;                                             // Ctrl+C must still work
    t.c_iflag &= ~static_cast<tcflag_t>(IXON);                     // Ctrl+S would freeze the screen
    t.c_cc[VMIN]  = 1;                                             // read() returns after 1 byte
    t.c_cc[VTIME] = 0;                                             // ... and never times out
    t.c_cc[VSUSP] = _POSIX_VDISABLE;                               // Ctrl+Z would leave a raw terminal behind
    return t;
}

RawMode::RawMode(int fd) : fd_(fd) {
    if (::tcgetattr(fd_, &saved_) != 0)
        throw std::runtime_error(std::string("tcgetattr: ") + std::strerror(errno));
    const termios raw = make_raw(saved_);
    if (::tcsetattr(fd_, TCSANOW, &raw) != 0)     // NOW, not FLUSH: keep type-ahead
        throw std::runtime_error(std::string("tcsetattr: ") + std::strerror(errno));
    active_ = true;
}

void RawMode::restore() noexcept {
    if (!active_) return;
    active_ = false;
    // DRAIN lets our last output reach the screen before the mode changes.
    while (::tcsetattr(fd_, TCSADRAIN, &saved_) != 0 && errno == EINTR) {}
}

// ── Signals ──────────────────────────────────────────────────────────────────
// The handlers may only do async-signal-safe things: a lock-free atomic store is one.
static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_winch{false};
static_assert(std::atomic<bool>::is_always_lock_free, "needed inside signal handlers");

static void on_stop(int)   { g_stop.store(true); }
static void on_winch(int)  { g_winch.store(true); }

static void install(int sig, void (*handler)(int), struct sigaction& old) {
    struct sigaction sa{};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                               // no SA_RESTART: poll() must return EINTR
    if (::sigaction(sig, &sa, &old) != 0)
        throw std::runtime_error(std::string("sigaction: ") + std::strerror(errno));
}

SignalGuard::SignalGuard() {
    g_stop.store(false);
    g_winch.store(false);
    install(SIGINT,   on_stop,  old_int_);
    install(SIGTERM,  on_stop,  old_term_);
    install(SIGHUP,   on_stop,  old_hup_);
    install(SIGWINCH, on_winch, old_winch_);
}

SignalGuard::~SignalGuard() {
    ::sigaction(SIGINT,   &old_int_,   nullptr);
    ::sigaction(SIGTERM,  &old_term_,  nullptr);
    ::sigaction(SIGHUP,   &old_hup_,   nullptr);
    ::sigaction(SIGWINCH, &old_winch_, nullptr);
}

bool SignalGuard::stop_requested() { return g_stop.load(); }
bool SignalGuard::take_resize()    { return g_winch.exchange(false); }

// ── KeyDecoder ───────────────────────────────────────────────────────────────
static constexpr std::size_t MAX_CSI_PARAMS = 16;   // longer is garbage, not a key

static bool is_continuation(unsigned char b) { return (b & 0xC0) == 0x80; }

// Bytes in a UTF-8 character that starts with 'lead', or 0 if 'lead' cannot start one.
static std::size_t utf8_length(unsigned char lead) {
    if (lead >= 0xC2 && lead <= 0xDF) return 2;
    if (lead >= 0xE0 && lead <= 0xEF) return 3;
    if (lead >= 0xF0 && lead <= 0xF4) return 4;
    return 0;
}

void KeyDecoder::ground(unsigned char b, std::vector<Key>& out) {
    switch (b) {
    case 0x1B: state_ = State::Esc; return;
    case '\r': case '\n': out.push_back({KeyType::Enter, {}}); return;
    case 0x7F: case 0x08: out.push_back({KeyType::Backspace, {}}); return;
    case 0x01: out.push_back({KeyType::Home, {}}); return;          // Ctrl+A
    case 0x05: out.push_back({KeyType::End, {}}); return;           // Ctrl+E
    case 0x04: out.push_back({KeyType::EndOfInput, {}}); return;    // Ctrl+D
    case 0x15: out.push_back({KeyType::KillLine, {}}); return;      // Ctrl+U
    case 0x17: out.push_back({KeyType::DeleteWord, {}}); return;    // Ctrl+W
    case 0x0C: out.push_back({KeyType::Redraw, {}}); return;        // Ctrl+L
    default: break;
    }
    if (b < 0x20) return;                                           // other controls: ignored
    if (b < 0x80) { out.push_back({KeyType::Char, std::string(1, static_cast<char>(b))}); return; }
    utf8_len_ = utf8_length(b);                                     // stray 80..C1, F5..FF: ignored
    if (utf8_len_ == 0) return;
    seq_.assign(1, static_cast<char>(b));
    state_ = State::Utf8;
}

void KeyDecoder::finish_csi(unsigned char final_byte, std::vector<Key>& out) {
    // Modifiers (ESC[1;5C = Ctrl+Right) are ignored: the base key is what counts.
    switch (final_byte) {
    case 'A': out.push_back({KeyType::Up, {}});    return;
    case 'B': out.push_back({KeyType::Down, {}});  return;
    case 'C': out.push_back({KeyType::Right, {}}); return;
    case 'D': out.push_back({KeyType::Left, {}});  return;
    case 'H': out.push_back({KeyType::Home, {}});  return;
    case 'F': out.push_back({KeyType::End, {}});   return;
    case '~': break;
    default:  return;
    }
    switch (std::atoi(seq_.c_str())) {              // stops at the first ';'
    case 1: case 7: out.push_back({KeyType::Home, {}});   break;
    case 4: case 8: out.push_back({KeyType::End, {}});    break;
    case 3:         out.push_back({KeyType::Delete, {}}); break;
    default: break;
    }
}

void KeyDecoder::step(unsigned char b, std::vector<Key>& out) {
    switch (state_) {
    case State::Ground:
        ground(b, out);
        return;
    case State::Esc:
        if (b == '[') { state_ = State::Csi; seq_.clear(); return; }
        if (b == 'O') { state_ = State::Ss3; return; }
        out.push_back({KeyType::Escape, {}});       // ESC then something else: a lone ESC...
        state_ = State::Ground;
        if (b != 0x1B) ground(b, out);              // ...and the byte is an ordinary key
        else state_ = State::Esc;                   // ESC ESC: the second one may start a sequence
        return;
    case State::Csi:
        if (b >= 0x40 && b <= 0x7E) {               // final byte ends the sequence
            state_ = State::Ground;
            finish_csi(b, out);
        } else if (b >= 0x20 && b <= 0x3F) {        // parameter or intermediate byte
            if (seq_.size() < MAX_CSI_PARAMS) seq_ += static_cast<char>(b);   // beyond that: swallowed
        } else {
            state_ = State::Ground;                 // garbage: drop it, then treat 'b' normally
            step(b, out);
        }
        return;
    case State::Ss3:
        state_ = State::Ground;
        seq_.clear();
        if (b == 'A') out.push_back({KeyType::Up, {}});
        else if (b == 'B') out.push_back({KeyType::Down, {}});
        else if (b == 'C') out.push_back({KeyType::Right, {}});
        else if (b == 'D') out.push_back({KeyType::Left, {}});
        else if (b == 'H') out.push_back({KeyType::Home, {}});
        else if (b == 'F') out.push_back({KeyType::End, {}});
        return;
    case State::Utf8:
        if (!is_continuation(b)) {                  // truncated character: drop it, reprocess 'b'
            state_ = State::Ground;
            step(b, out);
            return;
        }
        seq_ += static_cast<char>(b);
        if (seq_.size() < utf8_len_) return;
        state_ = State::Ground;
        // sanitize_text() is the project's UTF-8 validator (overlongs, surrogates,
        // > U+10FFFF, C1 controls); a character it would change is not typed text.
        if (sanitize_text(seq_) == seq_) out.push_back({KeyType::Char, seq_});
        return;
    }
}

std::vector<Key> KeyDecoder::feed(const char* data, std::size_t n) {
    std::vector<Key> out;
    for (std::size_t i = 0; i < n; ++i) step(static_cast<unsigned char>(data[i]), out);
    return out;
}

std::vector<Key> KeyDecoder::flush() {
    std::vector<Key> out;
    if (state_ == State::Esc) out.push_back({KeyType::Escape, {}});
    state_ = State::Ground;
    seq_.clear();
    return out;
}
