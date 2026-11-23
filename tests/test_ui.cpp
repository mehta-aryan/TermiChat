// tests/test_ui.cpp – unit tests for the terminal UI: capability detection,
// raw mode, key decoding, the line editor, wrapping and every look of the
// renderer.  Expected values are written out by hand; where a time of day
// appears it comes from format_clock(), which is tested in test_text.cpp.

#include "check.hpp"
#include "../src/line_editor.hpp"
#include "../src/protocol.hpp"
#include "../src/render.hpp"
#include "../src/terminal.hpp"
#include "../src/text_utils.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <clocale>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

static bool g_utf8 = false;     // is a UTF-8 locale available for width measuring?

static void skip(const char* what) { std::printf("  SKIP: %s\n", what); }

static std::string sgr(const std::string& codes, const std::string& text) {
    return "\x1b[" + codes + "m" + text + "\x1b[0m";
}

static bool has_esc(const std::string& s) { return s.find('\x1b') != std::string::npos; }

static std::string repeat(char c, int n) { return std::string(static_cast<std::size_t>(n), c); }

// ── Keys as text, so expectations stay readable ──────────────────────────────
static std::string name_of(const Key& k) {
    switch (k.type) {
    case KeyType::Char:       return k.text;
    case KeyType::Enter:      return "<Enter>";
    case KeyType::Backspace:  return "<Bksp>";
    case KeyType::Delete:     return "<Del>";
    case KeyType::Left:       return "<Left>";
    case KeyType::Right:      return "<Right>";
    case KeyType::Up:         return "<Up>";
    case KeyType::Down:       return "<Down>";
    case KeyType::Home:       return "<Home>";
    case KeyType::End:        return "<End>";
    case KeyType::KillLine:   return "<Kill>";
    case KeyType::DeleteWord: return "<Word>";
    case KeyType::Redraw:     return "<Redraw>";
    case KeyType::EndOfInput: return "<EOF>";
    case KeyType::Escape:     return "<Esc>";
    }
    return "?";
}

static std::string show(const std::vector<Key>& keys) {
    std::string s;
    for (const Key& k : keys) s += name_of(k);
    return s;
}

static std::string decode(const std::string& bytes) {
    KeyDecoder d;
    return show(d.feed(bytes.data(), bytes.size()));
}

// ── Events ───────────────────────────────────────────────────────────────────
static constexpr int64_t TS = 1700000000;

static std::string hm() { return format_clock(TS); }

static ChatEvent msg(const std::string& from, const std::string& text) {
    ChatEvent ev;
    ev.kind = EventKind::Message; ev.timestamp = TS; ev.sender = from; ev.text = text;
    return ev;
}
static ChatEvent priv(const std::string& from, const std::string& to, const std::string& text) {
    ChatEvent ev;
    ev.kind = EventKind::Private; ev.timestamp = TS; ev.sender = from; ev.recipient = to; ev.text = text;
    return ev;
}
static ChatEvent sys(SystemKind k, const std::string& text) {
    ChatEvent ev;
    ev.kind = EventKind::System; ev.timestamp = TS; ev.system_kind = k; ev.text = text;
    return ev;
}
static ChatEvent users(const std::vector<std::string>& names) {
    ChatEvent ev;
    ev.kind = EventKind::UserList; ev.timestamp = TS; ev.users = names;
    return ev;
}

static Style fancy(bool colour, bool unicode, int width) {
    Style st;
    st.plain = false; st.colour = colour; st.unicode = unicode; st.width = width;
    return st;
}

// One event through a fresh RenderState.
static std::vector<std::string> lines_of(const ChatEvent& ev, const Style& st,
                                         const std::string& me = "bob") {
    RenderState rs;
    return render_event(ev, me, st, rs);
}

static std::string first_line(const ChatEvent& ev, const Style& st, const std::string& me = "bob") {
    const std::vector<std::string> l = lines_of(ev, st, me);
    return l.empty() ? std::string("<none>") : l[0];
}

// ═════════════════════════════════════════════════════════════════════════════
static void test_classify() {
    std::puts("Test 1: interactive / colour / UTF-8 decision");
    TermCaps c = classify_terminal(true, true, "xterm-256color", nullptr, true);
    check(c.interactive && c.colour && c.utf8, "two terminals, real TERM -> everything on");
    c = classify_terminal(false, true, "xterm", nullptr, true);
    check(!c.interactive && !c.colour && !c.utf8, "stdin not a terminal -> plain, no colour, no UTF-8");
    c = classify_terminal(true, false, "xterm", nullptr, true);
    check(!c.interactive && !c.colour && !c.utf8, "stdout not a terminal (redirected) -> plain");
    c = classify_terminal(true, true, "dumb", nullptr, true);
    check(!c.interactive, "TERM=dumb -> plain");
    c = classify_terminal(true, true, nullptr, nullptr, true);
    check(!c.interactive, "TERM unset -> plain");
    c = classify_terminal(true, true, "xterm", "1", true);
    check(c.interactive && !c.colour && c.utf8, "NO_COLOR=1 -> UI without colour");
    c = classify_terminal(true, true, "xterm", "", true);
    check(c.colour, "NO_COLOR set but empty does not count (no-color.org)");
    c = classify_terminal(true, true, "xterm", nullptr, false);
    check(c.interactive && c.colour && !c.utf8, "non-UTF-8 locale -> ASCII symbols");
}

static void test_make_raw() {
    std::puts("Test 2: raw-mode settings");
    termios t;
    std::memset(&t, 0xFF, sizeof(t));
    const termios r = make_raw(t);
    check(!(r.c_lflag & ICANON), "ICANON cleared (no line buffering)");
    check(!(r.c_lflag & ECHO), "ECHO cleared (we draw the text ourselves)");
    check(!(r.c_lflag & IEXTEN), "IEXTEN cleared (Ctrl+V reaches us)");
    check((r.c_lflag & ISIG) != 0, "ISIG kept: Ctrl+C still raises SIGINT");
    check(!(r.c_iflag & IXON), "IXON cleared (Ctrl+S cannot freeze the screen)");
    check(r.c_cc[VMIN] == 1 && r.c_cc[VTIME] == 0, "VMIN=1, VTIME=0");
    check(r.c_cc[VSUSP] == _POSIX_VDISABLE, "VSUSP disabled (Ctrl+Z cannot strand a raw terminal)");
    check((r.c_iflag & ICRNL) != 0 && (r.c_oflag & OPOST) != 0, "everything else is left alone");
}

static void test_pty() {
    std::puts("Test 3: RawMode and terminal_size on a pseudo-terminal");
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) {
        skip("no pseudo-terminal available");
        if (master >= 0) ::close(master);
        return;
    }
    const int slave = ::open(::ptsname(master), O_RDWR | O_NOCTTY);
    if (slave < 0) { skip("cannot open the pty slave"); ::close(master); return; }

    termios before{};
    ::tcgetattr(slave, &before);
    check((before.c_lflag & ICANON) && (before.c_lflag & ECHO), "a fresh pty starts in cooked mode");
    {
        RawMode raw(slave);
        termios now{};
        ::tcgetattr(slave, &now);
        check(!(now.c_lflag & ICANON) && !(now.c_lflag & ECHO) && (now.c_lflag & ISIG),
              "RawMode switched the device");
    }
    termios after{};
    ::tcgetattr(slave, &after);
    check(after.c_lflag == before.c_lflag && after.c_iflag == before.c_iflag &&
          std::memcmp(after.c_cc, before.c_cc, sizeof(after.c_cc)) == 0,
          "leaving the scope restores every flag and control character");

    bool threw = false;
    try {
        RawMode raw(slave);
        raw.restore();
        raw.restore();                              // twice must be harmless
        termios now{};
        ::tcgetattr(slave, &now);
        check(now.c_lflag == before.c_lflag, "restore() is idempotent");
    } catch (...) { threw = true; }
    check(!threw, "no exception on a real terminal");

    int devnull = ::open("/dev/null", O_RDWR);
    bool refused = false;
    try { RawMode raw(devnull); } catch (const std::exception&) { refused = true; }
    check(refused, "RawMode on something that is not a terminal throws");
    ::close(devnull);

    winsize ws{};
    ws.ws_col = 100; ws.ws_row = 30;
    ::ioctl(master, TIOCSWINSZ, &ws);
    const TermSize sz = terminal_size(slave);
    check(sz.cols == 100 && sz.rows == 30, "terminal_size reads the window size");
    devnull = ::open("/dev/null", O_RDWR);
    const TermSize fallback = terminal_size(devnull);
    check(fallback.cols == 80 && fallback.rows == 24, "no terminal -> 80x24");
    ::close(devnull);
    ::close(slave);
    ::close(master);
}

static void test_write_all() {
    std::puts("Test 4: write_all");
    std::signal(SIGPIPE, SIG_IGN);                  // a closed pipe must give an error, not kill us
    int fds[2];
    if (::pipe(fds) != 0) { skip("no pipe"); return; }
    ::fcntl(fds[1], F_SETFL, O_NONBLOCK);           // a full pipe now gives EAGAIN

    const std::string big(300000, 'x');             // far more than a pipe holds (64 KiB)
    std::string received;
    std::thread reader([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        char buf[4096];
        while (received.size() < big.size()) {
            const ssize_t n = ::read(fds[0], buf, sizeof(buf));
            if (n <= 0) break;
            received.append(buf, static_cast<std::size_t>(n));
        }
    });
    const bool ok = write_all(fds[1], big);
    reader.join();
    check(ok, "write_all reports success for a write that needed many rounds");
    check(received == big, "...and every byte arrived, in order");
    check(write_all(fds[1], std::string()), "an empty write is fine");
    ::close(fds[0]);
    check(!write_all(fds[1], "x"), "a closed reader -> false (EPIPE), not a crash");
    ::close(fds[1]);
}

static void test_signals() {
    std::puts("Test 5: SignalGuard");
    struct sigaction before{}, during{}, after{};
    ::sigaction(SIGTERM, nullptr, &before);
    {
        SignalGuard guard;
        check(!SignalGuard::stop_requested() && !SignalGuard::take_resize(), "flags start clear");
        std::raise(SIGWINCH);
        check(SignalGuard::take_resize(), "SIGWINCH sets the resize flag");
        check(!SignalGuard::take_resize(), "...which is consumed by take_resize()");
        std::raise(SIGTERM);
        check(SignalGuard::stop_requested(), "SIGTERM only sets the stop flag (we are still alive)");
        ::sigaction(SIGTERM, nullptr, &during);
        check(!(during.sa_flags & SA_RESTART), "installed without SA_RESTART so poll() wakes up");
    }
    ::sigaction(SIGTERM, nullptr, &after);
    check(after.sa_handler == before.sa_handler, "the previous handler is back after the guard");
    SignalGuard again;
    check(!SignalGuard::stop_requested(), "a new guard starts with clean flags");
}

static void test_keys() {
    std::puts("Test 6: key decoder");
    check(decode("abc") == "abc", "printable ASCII");
    check(decode("a b") == "a b", "space is a character");
    check(decode("\r") == "<Enter>", "CR is Enter");
    check(decode("\n") == "<Enter>", "LF is Enter");
    check(decode("\x7f") == "<Bksp>" && decode("\x08") == "<Bksp>", "DEL and Ctrl+H are Backspace");
    check(decode("\x1b[A\x1b[B\x1b[C\x1b[D") == "<Up><Down><Right><Left>", "arrow keys");
    check(decode("\x1bOA\x1bOB\x1bOC\x1bOD") == "<Up><Down><Right><Left>", "arrow keys, application mode");
    check(decode("\x1b[H") == "<Home>" && decode("\x1b[F") == "<End>", "ESC[H / ESC[F");
    check(decode("\x1bOH") == "<Home>" && decode("\x1bOF") == "<End>", "ESC O H / ESC O F");
    check(decode("\x1b[1~") == "<Home>" && decode("\x1b[7~") == "<Home>", "ESC[1~ and ESC[7~ are Home");
    check(decode("\x1b[4~") == "<End>" && decode("\x1b[8~") == "<End>", "ESC[4~ and ESC[8~ are End");
    check(decode("\x1b[3~") == "<Del>", "ESC[3~ is Delete");
    check(decode("\x1b[1;5C") == "<Right>", "a modifier (Ctrl+Right) gives the base key");
    check(decode("\x01\x05") == "<Home><End>", "Ctrl+A / Ctrl+E");
    check(decode("\x15\x17\x0c\x04") == "<Kill><Word><Redraw><EOF>", "Ctrl+U, Ctrl+W, Ctrl+L, Ctrl+D");
    check(decode("\t\x1a\x13\x11") == "", "Tab, Ctrl+Z, Ctrl+S, Ctrl+Q are ignored");
    check(decode(std::string("a\0b", 3)) == "ab", "NUL is ignored");
    check(decode("\x1b[200~hi\x1b[201~") == "hi", "bracketed-paste markers are ignored, the text kept");
    check(decode("\x1b[?25l") == "", "an unknown CSI is swallowed whole");
    check(decode("\x1b[11111111111111111111111111111m!") == "!", "an endless parameter list is swallowed, not typed");
    check(decode("\x1b[\x1b[A") == "<Up>", "ESC inside a CSI abandons it and starts a new sequence");

    KeyDecoder d;
    check(show(d.feed("\x1b", 1)) == "" && d.pending(), "split: a lone ESC waits");
    check(show(d.feed("[", 1)) == "" && d.pending(), "split: ESC [ still waits");
    check(show(d.feed("A", 1)) == "<Up>" && !d.pending(), "split: ...and completes as Up");
    check(show(d.feed("\x1b[3", 3)) == "" && d.pending(), "split: ESC[3 waits");
    check(show(d.feed("~x", 2)) == "<Del>x", "split: ~ completes Delete, x follows");

    check(show(d.feed("\x1b", 1)) == "" && d.pending(), "lone ESC: waiting");
    check(show(d.flush()) == "<Esc>" && !d.pending(), "lone ESC: flush() turns it into the Escape key");
    check(show(d.flush()) == "", "flush() with nothing pending does nothing");
    check(decode("\x1bx") == "<Esc>x", "ESC then a letter: Escape, then the letter");
    check(decode("\x1b\x1b[A") == "<Esc><Up>", "ESC ESC [ A: Escape, then Up");
    check(show(d.feed("\x1b[1", 3)) == "" && show(d.flush()) == "" && !d.pending(),
          "flush() drops half a sequence");

    check(decode("caf\xc3\xa9") == "caf\xc3\xa9", "2-byte character");
    check(decode("\xe2\x82\xac") == "\xe2\x82\xac", "3-byte character (euro)");
    check(decode("\xf0\x9f\x98\x80") == "\xf0\x9f\x98\x80", "4-byte character (emoji)");
    check(show(d.feed("\xc3", 1)) == "" && d.pending(), "split UTF-8: first byte waits");
    check(show(d.feed("\xa9", 1)) == "\xc3\xa9" && !d.pending(), "split UTF-8: second byte completes it");
    check(show(d.feed("\xf0\x9f", 2)) == "" && show(d.feed("\x98", 1)) == "" &&
          show(d.feed("\x80", 1)) == "\xf0\x9f\x98\x80", "emoji split over three reads");
    check(decode("\x80\xbf\xc0\xc1\xf5\xff") == "", "stray continuation and invalid lead bytes are ignored");
    check(decode("\xc3" "A") == "A", "a truncated character is dropped, the next byte is kept");
    check(decode("\xe2\x82\x1b[A") == "<Up>", "...also when the next byte starts an escape sequence");
    check(decode("\xed\xa0\x80") == "", "a UTF-16 surrogate encoded in UTF-8 is rejected");
    check(decode("\xe0\x80\x80") == "", "an overlong encoding is rejected");
    check(decode("\xf4\x90\x80\x80") == "", "above U+10FFFF is rejected");
    check(decode("\xc2\x85") == "", "a C1 control character is not typed text");
}

static void test_editor_basics() {
    std::puts("Test 7: line editor - editing multibyte text");
    LineEditor e(1024);
    check(e.insert("a") && e.insert("\xe6\x97\xa5") && e.insert("b"), "insert 'a', '日', 'b'");
    check(e.text() == "a\xe6\x97\xa5" "b" && e.cursor() == 5, "text and byte cursor (1+3+1)");
    e.move_left();  check(e.cursor() == 4, "left over 'b'");
    e.move_left();  check(e.cursor() == 1, "left over the 3-byte '日' in one step");
    e.move_left();  check(e.cursor() == 0, "left over 'a'");
    e.move_left();  check(e.cursor() == 0, "left at the start stays");
    e.move_right(); e.move_right();
    check(e.cursor() == 4, "right over 'a' and '日'");
    e.move_end();   e.move_right();
    check(e.cursor() == 5, "right at the end stays");

    e.backspace();
    check(e.text() == "a\xe6\x97\xa5" && e.cursor() == 4, "backspace removes 'b'");
    e.backspace();
    check(e.text() == "a" && e.cursor() == 1, "backspace removes the whole 3-byte character");
    e.backspace(); e.backspace();
    check(e.text().empty() && e.cursor() == 0, "backspace on an empty line does nothing");

    e.insert("x"); e.insert("\xc3\xa9"); e.insert("z");
    e.move_home();
    e.erase_forward();
    check(e.text() == "\xc3\xa9" "z" && e.cursor() == 0, "Delete removes the character under the cursor");
    e.erase_forward();
    e.erase_forward();
    e.erase_forward();
    check(e.text().empty(), "Delete at the end does nothing");

    e.insert("a"); e.insert("c");
    e.move_left();
    e.insert("\xf0\x9f\x98\x80");
    e.insert("b");
    check(e.text() == "a\xf0\x9f\x98\x80" "bc" && e.cursor() == 6, "insert in the middle (emoji, then 'b')");
    e.move_home();
    check(e.cursor() == 0, "Home");
    e.kill_line();
    check(e.text().empty() && e.cursor() == 0, "Ctrl+U throws the whole line away");

    check(utf8_prev("a\xe6\x97\xa5", 4) == 1 && utf8_next("a\xe6\x97\xa5", 1) == 4, "utf8_prev / utf8_next");
    check(utf8_prev("abc", 0) == 0 && utf8_next("abc", 3) == 3 && utf8_prev("abc", 99) == 2,
          "utf8_prev / utf8_next clamp");
}

static void test_editor_words_and_limits() {
    std::puts("Test 8: line editor - word delete, refused input, byte limit");
    LineEditor e(1024);
    for (const char* ch : {"h","e","l","l","o"," ","w","\xc3\xb6","r","l","d"," "," "}) e.insert(ch);
    e.delete_word();
    check(e.text() == "hello ", "Ctrl+W removes trailing spaces and the word before them");
    e.delete_word();
    check(e.text().empty(), "Ctrl+W again removes 'hello '");
    e.delete_word();
    check(e.text().empty(), "Ctrl+W on an empty line does nothing");

    for (const char* ch : {"f","o","o"," ","b","a","r"," ","b","a","z"}) e.insert(ch);
    for (int i = 0; i < 4; ++i) e.move_left();                 // after "bar"
    e.delete_word();
    check(e.text() == "foo  baz" && e.cursor() == 4, "Ctrl+W in the middle keeps the text after the cursor");

    LineEditor f(1024);
    check(!f.insert("") && !f.insert("\x1b") && !f.insert("\n") && !f.insert("\t") && !f.insert("\x7f"),
          "empty input and control characters are refused");
    check(!f.insert("\xc3") && !f.insert("\x80") && !f.insert("ab") && !f.insert("\xc3\xa9\xc3\xa9"),
          "truncated, stray and multi-character input is refused");
    check(!f.insert("\xe2" "ab"), "a lead byte followed by non-continuation bytes is refused");
    check(f.text().empty(), "...and none of it changed the buffer");

    LineEditor g(5);
    for (const char* ch : {"a","b","c","d"}) g.insert(ch);
    check(!g.insert("\xc3\xa9") && g.text() == "abcd", "a 2-byte character that would pass the limit is refused");
    check(g.insert("e") && g.text() == "abcde", "a 1-byte character that fits is accepted");
    check(!g.insert("f") && g.text() == "abcde", "at the limit further input is ignored");
    g.backspace();
    check(g.insert("\xc3\xa9") == false, "...an 'é' still needs 2 bytes, only 1 is free");
    g.backspace();
    check(g.insert("\xc3\xa9") && g.text() == "abc\xc3\xa9", "after freeing 2 bytes it fits");

    LineEditor h(MAX_CHAT_TEXT);
    int accepted = 0;
    for (int i = 0; i < 5000; ++i) accepted += h.insert("x") ? 1 : 0;
    check(accepted == static_cast<int>(MAX_CHAT_TEXT) && h.text().size() == MAX_CHAT_TEXT,
          "5000 keystrokes into a MAX_CHAT_TEXT editor: exactly 1024 bytes kept, no crash");
    for (int i = 0; i < 600; ++i) h.insert("\xe6\x97\xa5");
    check(h.text().size() == MAX_CHAT_TEXT, "...and a CJK flood cannot overflow it either");
}

static void test_editor_history() {
    std::puts("Test 9: line editor - history");
    LineEditor e(1024);
    auto type = [&](const std::string& s) { for (char c : s) e.insert(std::string(1, c)); };
    check(e.submit().empty(), "submit on an empty editor returns an empty line");
    e.history_prev();
    check(e.text().empty(), "Up with no history does nothing");

    type("one");   check(e.submit() == "one" && e.text().empty() && e.cursor() == 0, "submit returns and clears");
    type("two");   e.submit();
    type("three"); e.submit();
    e.history_prev(); check(e.text() == "three" && e.cursor() == 5, "Up -> newest, cursor at the end");
    e.history_prev(); check(e.text() == "two", "Up -> older");
    e.history_prev(); check(e.text() == "one", "Up -> oldest");
    e.history_prev(); check(e.text() == "one", "Up at the oldest stays");
    e.history_next(); check(e.text() == "two", "Down -> newer");
    e.history_next(); check(e.text() == "three", "Down -> newest");
    e.history_next(); check(e.text().empty(), "Down past the newest restores the (empty) draft");
    e.history_next(); check(e.text().empty(), "Down when not browsing does nothing");

    type("dra");
    e.history_prev(); check(e.text() == "three", "Up with an unfinished line shows history...");
    e.history_next(); check(e.text() == "dra", "...and Down brings the unfinished line back");
    e.kill_line();

    type("three"); e.submit();
    type("   ");   e.submit();
    e.history_prev(); check(e.text() == "three", "a repeat of the last line is not stored twice");
    e.history_prev(); check(e.text() == "two", "...and a blank line is not stored at all");

    LineEditor big(1024);
    for (int i = 0; i < 60; ++i) {
        for (char c : "m" + std::to_string(i)) big.insert(std::string(1, c));
        big.submit();
    }
    for (int i = 0; i < 200; ++i) big.history_prev();
    check(big.text() == "m10", "only the last 50 lines are kept (m10 .. m59)");
}

static void test_wrap() {
    std::puts("Test 10: wrapping");
    check(wrap_text("hello world foo", 11, 11) == std::vector<std::string>({"hello world", "foo"}),
          "breaks at a space, line exactly full");
    check(wrap_text("aaa bbb ccc", 7, 3) == std::vector<std::string>({"aaa bbb", "ccc"}),
          "the first line has its own width");
    check(wrap_text("", 10, 10) == std::vector<std::string>({""}), "empty text -> one empty line");
    check(wrap_text("  a  b  ", 1, 1) == std::vector<std::string>({"a", "b"}),
          "spaces at the edges and at breaks are dropped");
    check(wrap_text("a   b", 10, 10) == std::vector<std::string>({"a   b"}), "spaces inside a line are kept");
    check(wrap_text("ab", 0, 0) == std::vector<std::string>({"a", "b"}), "width 0 is treated as 1");
    check(wrap_text("abcdefghij", 4, 4) == std::vector<std::string>({"abcd", "efgh", "ij"}),
          "an overlong word is cut");
    check(wrap_text("hi abcdefghij", 4, 4) == std::vector<std::string>({"hi", "abcd", "efgh", "ij"}),
          "...starting on a fresh line after the short word");
    check(wrap_text("ab cdefghij", 5, 4) == std::vector<std::string>({"ab", "cdef", "ghij"}),
          "a long word that does not fit the rest of a line moves down first");
    check(wrap_text("\x1b[2mhello world\x1b[0m", 5, 5) ==
          std::vector<std::string>({"\x1b[2mhello\x1b[0m", "\x1b[2mworld\x1b[0m"}),
          "a colour is closed at a break and reopened on the next line");
    check(wrap_text("\x1b[1;32mab\x1b[0m cd", 5, 5) == std::vector<std::string>({"\x1b[1;32mab\x1b[0m cd"}),
          "colour sequences count as zero columns");

    if (!g_utf8) { skip("wide characters (no UTF-8 locale)"); return; }
    check(display_width("abc") == 3 && display_width("caf\xc3\xa9") == 4, "display_width: ASCII and 'é'");
    check(display_width("\xe6\x97\xa5\xe6\x9c\xac") == 4, "display_width: two CJK characters are 4 columns");
    check(display_width("\xf0\x9f\x98\x80") == 2, "display_width: an emoji is 2 columns");
    check(display_width("e\xcc\x81") == 1, "display_width: a combining accent adds nothing");
    check(display_width(sgr("1;31", "ab")) == 2, "display_width ignores colour sequences");
    check(wrap_text("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", 5, 5) ==
          std::vector<std::string>({"\xe6\x97\xa5\xe6\x9c\xac", "\xe8\xaa\x9e\xe6\x97\xa5",
                                    "\xe6\x9c\xac\xe8\xaa\x9e"}),
          "wide characters: 2 per 5-column line (a third would need 6)");
    check(wrap_text("\xe6\x97\xa5\xe6\x9c\xac", 1, 1) == std::vector<std::string>({"\xe6\x97\xa5", "\xe6\x9c\xac"}),
          "a character wider than the line still gets placed (one per line)");
    check(truncate_cols("hello", 3, "") == "hel" && truncate_cols("hello", 4, "\xe2\x80\xa6") == "hel\xe2\x80\xa6",
          "truncate_cols, with and without a tail");
    check(truncate_cols("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", 5, "") == "\xe6\x97\xa5\xe6\x9c\xac",
          "truncate_cols never splits a wide character");
    check(truncate_cols("abc", 5, "...") == "abc" && truncate_cols("abc", 0, "") == "", "no cut needed / no room");
}

static void test_name_colours() {
    std::puts("Test 11: name colours");
    check(name_codes("alice") == name_codes("alice"), "deterministic");
    check(name_codes("Alice") == name_codes("aLICE"), "case-insensitive");
    check(name_codes("alice").rfind("1;38;5;", 0) == 0, "bold + a 256-colour code");
    std::vector<std::string> seen;
    bool never_red = true;
    const int reds[] = {1, 9, 52, 88, 124, 125, 160, 161, 166, 167, 196, 197, 198, 202, 203, 204};
    for (int i = 0; i < 2000; ++i) {
        const std::string codes = name_codes("user" + std::to_string(i));
        const int n = std::atoi(codes.substr(7).c_str());
        for (int r : reds) if (n == r) never_red = false;
        if (codes == "1;31" || codes == "1;91") never_red = false;
        if (std::find(seen.begin(), seen.end(), codes) == seen.end()) seen.push_back(codes);
    }
    check(never_red, "2000 different names: never a red colour");
    check(seen.size() >= 8, "...and the palette is really used (8+ different colours)");
    check(name_codes("bob") != "1;32" && name_codes("alice") != "1;32", "a normal name never gets 'you' green");
}

static void test_sanitize() {
    std::puts("Test 12: display-side sanitisation");
    ChatEvent ev = msg("al\x1b[31mice", "hi \x1b]0;pwned\x07 there\x1b[2J");
    ev.recipient = "bo\x07" "b";
    ev.users = {"a\x1b[0m", "\x1b\x07", "ok"};
    sanitize_event(ev);
    check(ev.sender == "al[31mice", "ESC removed from the sender");
    check(ev.text == "hi ]0;pwned there[2J", "ESC and BEL removed from the text");
    check(ev.recipient == "bob", "BEL removed from the recipient");
    check(ev.users[0] == "a[0m" && ev.users[2] == "ok", "controls removed from user-list names");
    check(ev.users[1] == "?", "a name that is nothing but controls becomes '?'");

    ChatEvent s = sys(SystemKind::Info, "caf\xc3\xa9 \xff \xc2\x9b" "31m end");
    sanitize_event(s);
    check(s.text == "caf\xc3\xa9 ? 31m end", "invalid UTF-8 -> '?', C1 control dropped, valid UTF-8 kept");
    check(s.sender.empty() && s.recipient.empty(), "unused fields stay empty (not turned into '?')");

    ChatEvent t = sys(SystemKind::Info, std::string("a\0b", 3));
    sanitize_event(t);
    check(t.text == "ab", "NUL dropped");

    ChatEvent hostile = msg("evil\x1b[1m", "x\x1b[31my");
    sanitize_event(hostile);
    const Style st = fancy(false, false, 80);
    check(!has_esc(first_line(hostile, st)), "after sanitising, the rendered line contains no ESC");
}

static void test_plain_render() {
    std::puts("Test 13: plain layout is byte-for-byte the old format");
    Style st;                                       // plain by default
    check(first_line(msg("alice", "hi there"), st) == "[" + hm() + "] alice: hi there", "message");
    check(first_line(msg("bob", "echo"), st) == "[" + hm() + "] bob: echo", "own message keeps the real name");
    check(first_line(priv("alice", "bob", "psst"), st) == "[" + hm() + "] [private] alice -> bob: psst",
          "private message");
    for (SystemKind k : {SystemKind::Info, SystemKind::Join, SystemKind::Leave, SystemKind::Error,
                         SystemKind::Shutdown})
        check(first_line(sys(k, "text here"), st) == "*** text here", "system line");
    check(first_line(users({"alice", "bob", "carol"}), st) == "*** Online (3): alice, bob, carol",
          "user list, no '(you)' in plain mode");
    check(first_line(users({}), st) == "*** Online (0): ", "empty user list");
    check(lines_of(msg("alice", std::string(500, 'x')), st).size() == 1, "plain never wraps");

    Style loud = st;
    loud.colour = true; loud.unicode = true; loud.width = 10;
    check(first_line(msg("alice", "hi"), loud) == "[" + hm() + "] alice: hi", "colour/unicode/width are ignored when plain");

    const std::vector<std::string> help = render_help(st, TS);
    check(help == std::vector<std::string>({
              "*** Commands:",
              "***   /help               show this help",
              "***   /users              list who is online",
              "***   /msg <user> <text>  send a private message to one user",
              "***   /quit               leave the chat (Ctrl+D works too)"}),
          "plain /help is exactly the old text (no /clear)");
}

static void test_fancy_ascii() {
    std::puts("Test 14: fancy layout, ASCII, no colour");
    const Style st = fancy(false, false, 80);
    const std::string t = " " + hm() + "  ";
    RenderState rs;
    check(render_event(msg("alice", "hello everyone"), "bob", st, rs)[0] == t + "alice > hello everyone",
          "other user's message");
    check(render_event(msg("bob", "hi alice!"), "bob", st, rs)[0] == t + "you   > hi alice!",
          "own message: 'you', padded to the longest name seen");
    check(render_event(msg("BOB", "case"), "bob", st, rs)[0] == t + "you   > case",
          "own name is recognised in any case");
    check(first_line(msg("al", "x"), st) == t + "al  > x", "a fresh state pads to 'you' (3) at least");
    check(first_line(sys(SystemKind::Join, "carol joined the chat"), st) == t + "-> carol joined the chat", "join");
    check(first_line(sys(SystemKind::Leave, "carol left the chat"), st) == t + "<- carol left the chat", "leave");
    check(first_line(sys(SystemKind::Info, "Welcome"), st) == t + "* Welcome", "info");
    check(first_line(sys(SystemKind::Error, "No such user: dave"), st) == t + "! No such user: dave", "error");
    check(first_line(sys(SystemKind::Shutdown, "Server is shutting down"), st) == t + "! Server is shutting down",
          "shutdown");
    check(first_line(priv("alice", "bob", "psst"), st) == t + "* alice -> you: psst", "private to me");
    check(first_line(priv("bob", "alice", "ok"), st) == t + "* you -> alice: ok", "private from me");
    check(first_line(users({"alice", "bob", "carol"}), st) == t + "* Online (3): alice, bob (you), carol",
          "user list marks me with (you)");

    RenderState grow;
    render_event(msg("al", "x"), "bob", st, grow);
    check(grow.name_cols == 3, "state: 'you' is the minimum width");
    render_event(msg("bartholomew", "x"), "bob", st, grow);
    check(grow.name_cols == 11, "state: grows with the longest name");
    check(render_event(msg("al", "x"), "bob", st, grow)[0] == t + "al          > x",
          "...and later short names are padded to it");
    render_event(msg(repeat('n', 40), "x"), "bob", st, grow);
    check(grow.name_cols == MAX_NAME_COLS, "state: capped at 16");
    check(render_event(msg(repeat('n', 40), "x"), "bob", st, grow)[0] == t + repeat('n', 16) + " > x",
          "a longer name is cut to 16");
    check(first_line(msg("alice", ""), st) == t + "alice > ", "empty text does not crash");

    for (const ChatEvent& ev : {msg("a", "x"), priv("a", "b", "x"), sys(SystemKind::Error, "x"), users({"a"})})
        for (const std::string& l : lines_of(ev, st))
            check(!has_esc(l) && l.find('\n') == std::string::npos, "no ESC, no newline in a no-colour line");

    const std::vector<std::string> help = render_help(st, TS);
    check(help.size() == 6 && help[0] == t + "* Commands:", "fancy help: header + 5 commands");
    check(help[1] == "          /help               show this help", "fancy help: two aligned columns");
    check(help[4] == "          /clear              clear the screen", "fancy help lists /clear");
    check(help[3] == "          /msg <user> <text>  send a private message to one user", "...widest command fits");
}

static void test_fancy_unicode_colour() {
    std::puts("Test 15: fancy layout, UTF-8 symbols and colour");
    if (!g_utf8) { skip("UTF-8 symbols (no UTF-8 locale)"); return; }
    const Style uni = fancy(false, true, 80);
    const std::string t = " " + hm() + "  ";
    check(first_line(msg("alice", "hello"), uni) == t + "alice \xe2\x9d\xaf hello", "chevron");
    check(first_line(sys(SystemKind::Join, "c joined"), uni) == t + "\xe2\x86\x92 c joined", "join arrow");
    check(first_line(sys(SystemKind::Leave, "c left"), uni) == t + "\xe2\x86\x90 c left", "leave arrow");
    check(first_line(sys(SystemKind::Info, "hello"), uni) == t + "\xe2\x80\xa2 hello", "info bullet");
    check(first_line(sys(SystemKind::Error, "bad"), uni) == t + "! bad", "error bang");
    check(first_line(priv("alice", "bob", "psst"), uni) == t + "\xe2\x9c\x89 alice \xe2\x86\x92 you: psst",
          "private: envelope and arrow");
    check(first_line(users({"alice", "bob"}), uni) == t + "\xe2\x97\x8f Online (2): alice, bob (you)", "online dot");

    const Style col = fancy(true, true, 80);
    const std::string tc = " " + sgr("2", hm()) + "  ";
    const std::string chev = sgr("2", "\xe2\x9d\xaf");
    check(first_line(msg("alice", "hello"), col) == tc + sgr(name_codes("alice"), "alice") + " " + chev + " hello",
          "message: dim time, hashed bold name, dim chevron");
    check(first_line(msg("bob", "hi"), col) == tc + sgr("1;32", "you") + " " + chev + " hi", "own name: bold green 'you'");
    check(first_line(sys(SystemKind::Join, "c joined"), col) ==
          tc + sgr("2;32", "\xe2\x86\x92") + " " + sgr("2;32", "c joined"), "join: dim green");
    check(first_line(sys(SystemKind::Leave, "c left"), col) ==
          tc + sgr("2;33", "\xe2\x86\x90") + " " + sgr("2;33", "c left"), "leave: dim yellow");
    check(first_line(sys(SystemKind::Info, "hi"), col) ==
          tc + sgr("2;36", "\xe2\x80\xa2") + " " + sgr("2;36", "hi"), "info: dim cyan");
    check(first_line(sys(SystemKind::Error, "bad"), col) == tc + sgr("31", "!") + " " + sgr("31", "bad"), "error: red");
    check(first_line(sys(SystemKind::Shutdown, "bye"), col) == tc + sgr("33", "!") + " " + sgr("33", "bye"),
          "shutdown: yellow");
    check(first_line(priv("alice", "bob", "psst"), col) ==
          tc + sgr("35", "\xe2\x9c\x89") + " " + sgr("35", "alice \xe2\x86\x92 you: ") + sgr("35", "psst"),
          "private: magenta");
    check(first_line(users({"alice", "bob"}), col) ==
          tc + sgr("2;36", "\xe2\x97\x8f") + " " + sgr("2;36", "Online (2): ") + sgr(name_codes("alice"), "alice")
          + ", " + sgr("1;32", "bob") + sgr("2", " (you)"),
          "user list: hashed names, green me, dim (you)");

    Style no_col = col;
    no_col.colour = false;
    for (const ChatEvent& ev : {msg("a", "x"), priv("a", "b", "x"), sys(SystemKind::Join, "x"), users({"a", "bob"})})
        check(!has_esc(first_line(ev, no_col)), "NO_COLOR: not one escape byte");
}

static void test_fancy_wrapping() {
    std::puts("Test 16: wrapping inside the layout");
    const Style st = fancy(false, false, 32);
    const std::string t = " " + hm() + "  ";
    const std::vector<std::string> l = lines_of(msg("alice", "aaaa bbbb cccc dddd eeee"), st);
    check(l.size() == 2 && l[0] == t + "alice > aaaa bbbb cccc" && l[1] == repeat(' ', 16) + "dddd eeee",
          "hand-checked: width 32, the continuation hangs under the text (16 columns)");

    const std::string long_text = "this is a rather long message that has to wrap over several lines, "
                                  "and every continuation line must stay aligned under the text";
    for (int width : {30, 40, 60, 80, 120}) {
        const Style s = fancy(false, false, width);
        const std::vector<std::string> lines = lines_of(msg("alice", long_text), s);
        // The head "HH:MM  alice > " is 16 columns; if that is over half the row the
        // continuation hangs at 10 instead (under the name), so the text keeps room.
        const std::size_t indent = 16 <= width / 2 ? 16 : 10;
        bool fits = true, hangs = true;
        std::string body;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (display_width(lines[i]) > width) fits = false;
            if (i > 0) {
                if (lines[i].compare(0, indent, repeat(' ', static_cast<int>(indent))) != 0) hangs = false;
                body += " " + lines[i].substr(indent);
            } else {
                body += lines[i].substr(t.size() + 8);       // "alice > " is 8 columns
            }
        }
        check(fits, ("width " + std::to_string(width) + ": no line is wider than the terminal").c_str());
        check(hangs, ("width " + std::to_string(width) + ": continuation lines hang under the text").c_str());
        check(body == long_text, ("width " + std::to_string(width) + ": no text lost or changed").c_str());
    }

    check(lines_of(msg("alice", long_text), fancy(false, false, 20)).size() == 1,
          "below 30 columns: one unwrapped line");
    check(lines_of(msg("alice", long_text), fancy(false, false, 29)).size() == 1, "29 columns: still unwrapped");
    check(lines_of(msg("alice", long_text), fancy(false, false, 30)).size() > 1, "30 columns: wraps");

    const Style narrow = fancy(false, false, 40);
    const std::vector<std::string> deep = lines_of(msg(repeat('n', 16), long_text), narrow);
    bool ok = deep.size() > 2;
    for (std::size_t i = 0; i < deep.size(); ++i) {
        if (display_width(deep[i]) > 40) ok = false;
        if (i > 0 && deep[i].compare(0, 10, repeat(' ', 10)) != 0) ok = false;
    }
    check(ok, "a 16-character name on a 40-column terminal: continuation hangs at 10, nothing overflows");

    check(lines_of(msg("alice", repeat('x', 200)), fancy(false, false, 40)).size() > 5, "an unbreakable word is cut");
    check(lines_of(sys(SystemKind::Info, long_text), fancy(false, false, 40)).size() > 2, "system lines wrap too");
    const std::vector<std::string> sl = lines_of(sys(SystemKind::Info, long_text), fancy(false, false, 40));
    check(sl.size() > 1 && sl[1].compare(0, 10, repeat(' ', 10)) == 0 && sl[1][10] != ' ',
          "system continuation hangs under the text (10 columns)");

    const Style col = fancy(true, false, 40);
    bool balanced = true;
    for (const std::string& line : lines_of(sys(SystemKind::Info, long_text), col)) {
        int opens = 0, resets = 0;
        for (std::size_t i = 0; i + 3 < line.size(); ++i) {
            if (line.compare(i, 4, "\x1b[0m") == 0) ++resets;
            else if (line[i] == '\x1b') ++opens;
        }
        if (opens != resets) balanced = false;
    }
    check(balanced, "coloured wrapped lines each close what they open");

    if (!g_utf8) { skip("CJK and emoji wrapping (no UTF-8 locale)"); return; }
    std::string cjk;
    for (int i = 0; i < 60; ++i) cjk += "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
    cjk += " ok \xf0\x9f\x98\x80\xf0\x9f\x98\x80";
    bool fits = true;
    for (const std::string& line : lines_of(msg("alice", cjk), fancy(false, false, 40)))
        if (display_width(line) > 40) fits = false;
    check(fits, "CJK and emoji text: every line within 40 columns");
}

static void test_banner() {
    std::puts("Test 17: banner");
    if (!g_utf8) { skip("banner (no UTF-8 locale)"); return; }
    const std::vector<std::string> b = render_banner("192.168.1.5", 12345, "bob", fancy(false, true, 80));
    check(b.size() == 5, "box: top, 3 text rows, bottom");
    check(b[0].rfind("\xe2\x95\xad", 0) == 0 && b[4].rfind("\xe2\x95\xb0", 0) == 0, "rounded corners");
    check(b[1].find("TermiChat") != std::string::npos && b[2].find("connected to 192.168.1.5:12345") != std::string::npos &&
          b[3].find("you are bob \xc2\xb7 type /help for commands") != std::string::npos, "the three lines of text");
    bool even = true;
    for (const std::string& l : b) if (display_width(l) != display_width(b[0])) even = false;
    check(even, "all box rows are equally wide");

    const std::vector<std::string> a = render_banner("h", 1, "bob", fancy(false, false, 80));
    check(a[0].rfind("+---", 0) == 0 && a[1].rfind("|  TermiChat", 0) == 0 && a[3].find("you are bob - type") != std::string::npos,
          "ASCII box and separator");

    const std::vector<std::string> narrow = render_banner("h", 1, "bob", fancy(false, false, 20));
    check(narrow.size() == 3 && narrow[0] == "TermiChat", "below 30 columns: no box");

    const std::vector<std::string> longhost = render_banner(repeat('h', 120), 1, "bob", fancy(false, false, 40));
    bool fits = true;
    for (const std::string& l : longhost) if (display_width(l) > 40) fits = false;
    check(fits && longhost[2].find("...") != std::string::npos, "a long host is cut to fit, with '...'");
    check(!has_esc(render_banner("h\x1b[2J", 1, "bob", fancy(false, false, 80))[2]), "the host is sanitised");

    const std::vector<std::string> col = render_banner("h", 1, "bob", fancy(true, true, 80));
    check(col[2].find("\x1b[") != std::string::npos, "colour mode styles the box");
}

static void test_input_row() {
    std::puts("Test 18: input row and horizontal scrolling");
    const Style st = fancy(false, false, 20);          // prompt "bob > " = 6 columns, 13 left for text
    std::size_t scroll = 0;
    InputRow r = render_input_row("bob", "hello", 5, scroll, st);
    check(r.text == "bob > hello" && r.cursor_col == 11 && scroll == 0, "short line: prompt + text, cursor after it");
    r = render_input_row("bob", "", 0, scroll, st);
    check(r.text == "bob > " && r.cursor_col == 6, "empty line");
    r = render_input_row("bob", "hello", 2, scroll, st);
    check(r.text == "bob > hello" && r.cursor_col == 8, "cursor in the middle");

    const std::string line = "abcdefghijklmnopqrstuvwxyz0123";      // 30 characters
    scroll = 0;
    r = render_input_row("bob", line, 30, scroll, st);
    check(scroll == 17 && r.text == "bob > rstuvwxyz0123" && r.cursor_col == 19,
          "long line, cursor at the end: scrolled so the cursor sits in the last column");
    check(display_width(r.text) <= 19, "...and the row never uses the last column (no auto-wrap)");
    r = render_input_row("bob", line, 29, scroll, st);
    check(scroll == 17 && r.cursor_col == 18, "cursor one to the left: the window does not move");
    r = render_input_row("bob", line, 17, scroll, st);
    check(scroll == 11 && r.text == "bob > lmnopqrstuvwx" && r.cursor_col == 12,
          "cursor on the left edge: the window moves back to show context");
    r = render_input_row("bob", line, 0, scroll, st);
    check(scroll == 0 && r.text == "bob > abcdefghijklm" && r.cursor_col == 6, "Home: back to the start");

    scroll = 50;
    r = render_input_row("bob", "abc", 3, scroll, st);
    check(r.text == "bob > abc" && scroll == 0, "a stale scroll beyond the text (after the line shrank) is repaired");

    if (g_utf8) {
        const std::string cjk = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";   // 6 x 2 columns
        scroll = 0;
        r = render_input_row("bob", cjk, cjk.size(), scroll, fancy(false, false, 12));
        check(r.text == "> \xe8\xaa\x9e\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e" && r.cursor_col == 10,
              "narrow terminal: the name is dropped for room, wide characters scroll whole");
        scroll = 1;                                         // inside the first 3-byte character
        render_input_row("bob", cjk, 0, scroll, st);
        check(scroll == 0, "a scroll offset in the middle of a character is moved to its start");
    } else {
        skip("wide characters in the input row (no UTF-8 locale)");
    }

    const Style col = fancy(true, false, 20);
    scroll = 0;
    r = render_input_row("bob", "hi", 2, scroll, col);
    check(r.text == sgr("1", "bob") + " " + sgr("2", ">") + " hi" && r.cursor_col == 8, "prompt: bold name, dim chevron");

    // Whatever the buffer, cursor, width and earlier scroll: the cursor is on screen and the row fits.
    std::mt19937 rng(12345);
    const std::vector<std::string> pieces = {"a", "b", " ", "xyz", "\xc3\xa9", "\xe6\x97\xa5", "\xf0\x9f\x98\x80"};
    bool holds = true;
    for (int round = 0; round < 3000 && holds; ++round) {
        std::string buf;
        std::vector<std::size_t> boundaries = {0};
        for (int i = static_cast<int>(rng() % 40); i > 0; --i) {
            buf += pieces[rng() % (g_utf8 ? pieces.size() : 4)];
            boundaries.push_back(buf.size());
        }
        const std::size_t cur = boundaries[rng() % boundaries.size()];
        std::size_t sc = rng() % (buf.size() + 5);
        const int width = 10 + static_cast<int>(rng() % 60);
        const InputRow row = render_input_row("bob", buf, cur, sc, fancy(false, false, width));
        if (display_width(row.text) > width - 1 || row.cursor_col > width - 1 || row.cursor_col < 2 ||
            sc > cur || (sc < buf.size() && sc != utf8_prev(buf, utf8_next(buf, sc))))
            holds = false;
    }
    check(holds, "3000 random lines/cursors/widths: row fits, cursor visible, scroll on a boundary");
}

int main() {
    // Width measuring needs a UTF-8 locale for anything beyond ASCII.
    for (const char* name : {"C.UTF-8", "en_US.UTF-8", "C.utf8"})
        if (std::setlocale(LC_ALL, name) != nullptr) { g_utf8 = true; break; }
    if (!g_utf8) std::puts("(no UTF-8 locale available: wide-character tests will be skipped)");

    test_classify();
    test_make_raw();
    test_pty();
    test_write_all();
    test_signals();
    test_keys();
    test_editor_basics();
    test_editor_words_and_limits();
    test_editor_history();
    test_wrap();
    test_name_colours();
    test_sanitize();
    test_plain_render();
    test_fancy_ascii();
    test_fancy_unicode_colour();
    test_fancy_wrapping();
    test_banner();
    test_input_row();
    return summary();
}
