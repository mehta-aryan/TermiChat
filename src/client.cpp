// client.cpp – Client implementation.

#include "client.hpp"
#include "line_editor.hpp"
#include "render.hpp"
#include "text_utils.hpp"

#include <sys/socket.h>   // socket, connect, shutdown
#include <netdb.h>        // getaddrinfo, freeaddrinfo
#include <unistd.h>       // read, close
#include <poll.h>         // poll

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>         // fprintf
#include <cstring>        // strerror
#include <exception>
#include <mutex>
#include <optional>
#include <string>

// ── Screen ────────────────────────────────────────────────────────────────────
// The ONLY object that writes chat output.  One mutex serialises the receiver
// thread (incoming events) and the main thread (typing, notices); every update
// is built as one string and sent with a single write_all(), so a line can
// never be torn by a keystroke redraw or the other way round.
//
// In interactive mode the last terminal row is the input row.  Printing
// something therefore means: erase that row, print the lines, redraw the row.
class Screen {
public:
    explicit Screen(const TermCaps& caps) {
        style_.plain   = !caps.interactive;
        style_.colour  = caps.colour;
        style_.unicode = caps.utf8;
        if (caps.interactive) style_.width = terminal_size(STDOUT_FILENO).cols;
    }

    // Fixed after construction, so safe to read from any thread.
    bool interactive() const { return !style_.plain; }

    // Enters the UI: signal handlers (both modes, so Ctrl+C and SIGTERM are
    // always a graceful quit), then, in interactive mode only, raw mode, banner
    // and prompt.  stop() undoes it; both are safe to call twice.
    void start(const std::string& me, const std::string& host, uint16_t port) {
        std::lock_guard<std::mutex> lk(mu_);
        me_ = me;
        host_ = host;
        port_ = port;
        signals_.emplace();
        if (style_.plain) return;
        raw_.emplace(STDIN_FILENO);
        style_.width = terminal_size(STDOUT_FILENO).cols;
        active_ = true;
        write_all(STDOUT_FILENO, banner_locked() + input_row_locked());
    }

    void stop() {
        std::lock_guard<std::mutex> lk(mu_);
        if (active_) {
            active_ = false;
            write_all(STDOUT_FILENO, ERASE_ROW);   // leave a clean row for what comes next
            raw_.reset();                          // terminal back to normal...
        }
        signals_.reset();                          // ...and the default signal behaviour
    }

    void show(const ChatEvent& ev) {
        std::lock_guard<std::mutex> lk(mu_);
        emit_locked(render_event(ev, me_, style_, names_));
    }

    void show_help() {
        std::lock_guard<std::mutex> lk(mu_);
        emit_locked(render_help(style_, unix_now()));
    }

    void print(const std::string& line) {
        std::lock_guard<std::mutex> lk(mu_);
        emit_locked({line});
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        update_size_locked();
        write_all(STDOUT_FILENO, "\x1b[2J\x1b[H" + banner_locked() + input_row_locked());
    }

    // Applies one key to the input line.  Returns true when it completed a
    // line, which is then in 'submitted' (Ctrl+D on an empty line gives "/quit").
    bool apply_key(const Key& key, std::string& submitted) {
        std::lock_guard<std::mutex> lk(mu_);
        update_size_locked();
        bool done = false;
        switch (key.type) {
        case KeyType::Char:       editor_.insert(key.text); break;
        case KeyType::Backspace:  editor_.backspace();      break;
        case KeyType::Delete:     editor_.erase_forward();  break;
        case KeyType::Left:       editor_.move_left();      break;
        case KeyType::Right:      editor_.move_right();     break;
        case KeyType::Home:       editor_.move_home();      break;
        case KeyType::End:        editor_.move_end();       break;
        case KeyType::Up:         editor_.history_prev();   break;
        case KeyType::Down:       editor_.history_next();   break;
        case KeyType::KillLine:   editor_.kill_line();      break;
        case KeyType::DeleteWord: editor_.delete_word();    break;
        case KeyType::Enter:
            submitted = editor_.submit();
            done = !submitted.empty();
            break;
        case KeyType::EndOfInput:
            if (editor_.text().empty()) { submitted = "/quit"; done = true; }
            else editor_.erase_forward();                   // like a shell: Ctrl+D deletes
            break;
        case KeyType::Redraw:
        case KeyType::Escape:     break;                    // the redraw below is the whole job
        }
        write_all(STDOUT_FILENO, redraw_locked());
        return done;
    }

    // Called regularly from the main loop: picks up a resize.
    void refresh() {
        std::lock_guard<std::mutex> lk(mu_);
        if (update_size_locked()) write_all(STDOUT_FILENO, redraw_locked());
    }

private:
    static constexpr const char* ERASE_ROW = "\r\x1b[K";    // cursor to column 0, clear the row

    std::mutex   mu_;
    Style        style_;
    RenderState  names_;
    std::string  me_, host_;
    uint16_t     port_ = 0;
    LineEditor   editor_{MAX_CHAT_TEXT};
    std::size_t  scroll_ = 0;                // first visible byte of the input line
    bool         active_ = false;            // raw mode is on and the input row exists
    std::optional<SignalGuard> signals_;
    std::optional<RawMode>     raw_;

    std::string eol() const { return style_.plain ? "\n" : "\r\n"; }

    // Re-reads the window size after a SIGWINCH.  Returns true if it changed.
    bool update_size_locked() {
        if (!active_ || !SignalGuard::take_resize()) return false;
        style_.width = terminal_size(STDOUT_FILENO).cols;
        return true;
    }

    std::string lines_locked(const std::vector<std::string>& lines) const {
        std::string out;
        for (const std::string& l : lines) out += l + eol();
        return out;
    }

    std::string banner_locked() const {
        return lines_locked(render_banner(host_, port_, me_, style_));
    }

    // Prompt + visible text, with the terminal cursor moved to the edit position.
    std::string input_row_locked() {
        const InputRow row = render_input_row(me_, editor_.text(), editor_.cursor(), scroll_, style_);
        return row.text + "\x1b[" + std::to_string(row.cursor_col + 1) + "G";
    }

    std::string redraw_locked() {
        return active_ ? ERASE_ROW + input_row_locked() : std::string();
    }

    void emit_locked(const std::vector<std::string>& lines) {
        update_size_locked();
        std::string out;
        if (active_) out += ERASE_ROW;
        out += lines_locked(lines);
        if (active_) out += input_row_locked();
        write_all(STDOUT_FILENO, out);       // ONE write: nothing can end up between the parts
    }
};

// Leaves the UI when it goes out of scope, so every way out of Client::run()
// (return, exception) puts the terminal back.
class ScreenSession {
public:
    ScreenSession(Screen& screen, const std::string& me, const std::string& host, uint16_t port)
        : screen_(screen) { screen_.start(me, host, port); }
    ~ScreenSession() { screen_.stop(); }
    ScreenSession(const ScreenSession&)            = delete;
    ScreenSession& operator=(const ScreenSession&) = delete;
private:
    Screen& screen_;
};

// Explains why a frame could not be received (shared by join and the receiver).
static std::string describe_failure(FrameStatus st, int err) {
    switch (st) {
    case FrameStatus::PEER_CLOSED:          return "Server closed the connection";
    case FrameStatus::PEER_CLOSED_MIDFRAME: return "Server closed the connection (mid-frame)";
    case FrameStatus::IO_ERROR:             return std::string("Connection error: ") + std::strerror(err);
    default:                                return "Protocol error from server";
    }
}

// ── Constructor: resolve host and connect ─────────────────────────────────────
Client::Client(const std::string& host, uint16_t port, std::string username)
    : sock_(-1), host_(host), port_(port), username_(std::move(username))
{
    // getaddrinfo() performs DNS resolution and returns a linked list of
    // candidate addresses.  Using it (instead of gethostbyname) is thread-safe
    // and supports both IPv4 and IPv6 without code changes.
    addrinfo hints{};
    hints.ai_family   = AF_INET;      // IPv4 only
    hints.ai_socktype = SOCK_STREAM;  // TCP

    addrinfo* res = nullptr;
    std::string port_str = std::to_string(port);
    int gai_err = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (gai_err != 0) {
        // getaddrinfo does not use errno; it has its own error codes.
        throw NetError("getaddrinfo(" + host + ")", gai_strerror(gai_err));
    }

    // Try each result in order until one connects successfully.
    int saved_errno = 0;
    bool connected = false;
    for (addrinfo* rp = res; rp != nullptr && !connected; rp = rp->ai_next) {
        // Socket owns the fd: a candidate that fails to connect is closed
        // automatically when 'candidate' goes out of scope.
        Socket candidate(::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol));
        if (candidate.fd() < 0) { saved_errno = errno; continue; }

        // connect() initiates the TCP three-way handshake.  It blocks until
        // the kernel receives the SYN-ACK from the server and sends the final
        // ACK, or until it times out or is refused.
        if (::connect(candidate.fd(), rp->ai_addr, rp->ai_addrlen) == 0) {
            sock_ = std::move(candidate);
            connected = true;
        } else {
            saved_errno = errno;
        }
    }
    ::freeaddrinfo(res);  // must always free, even on error

    if (!connected) {
        errno = saved_errno;
        throw NetError("connect() to " + host + ":" + port_str);
    }

    screen_ = std::make_unique<Screen>(detect_terminal());
    // The fancy UI shows the address in its banner instead.
    if (!screen_->interactive()) screen_->print("Connected to " + host + ":" + port_str);
}

Client::~Client() = default;

void Client::notice(SystemKind kind, const std::string& text) {
    ChatEvent ev;
    ev.kind        = EventKind::System;
    ev.timestamp   = unix_now();
    ev.system_kind = kind;
    ev.text        = text;
    screen_->show(ev);
}

// ── Joining ───────────────────────────────────────────────────────────────────
bool Client::join() {
    if (send_frame(sock_.fd(), MsgType::JOIN, username_) != FrameStatus::OK) {
        notice(SystemKind::Error, std::string("Send failed: ") + std::strerror(errno));
        return false;
    }

    // Nothing else is running yet, so we can simply block here until the
    // server answers.  A server that closes right after JOIN_ERR ends this
    // wait with the reason already in hand; no thread is left hanging.
    MsgType     type;
    std::string payload;
    const FrameStatus st  = recv_frame(sock_.fd(), type, payload);
    const int         err = errno;

    if (st != FrameStatus::OK) {
        notice(SystemKind::Error, describe_failure(st, err));
        return false;
    }
    if (type == MsgType::JOIN_ERR) {
        // The reason is server-supplied text: sanitise before it hits the terminal.
        std::fprintf(stderr, "Error: %s\n", sanitize_text(payload).c_str());
        return false;
    }
    if (type != MsgType::JOIN_OK || check_username(payload) != NameCheck::Ok) {
        notice(SystemKind::Error, describe_failure(FrameStatus::PROTOCOL_VIOLATION, 0));
        return false;
    }
    username_ = payload;     // the canonical spelling chosen by the server
    return true;
}

// ── Receiver thread ───────────────────────────────────────────────────────────
void Client::receiver_loop() {
    // An exception must not escape a thread (that would be std::terminate with
    // the terminal still in raw mode), so it ends the session like any failure.
    try {
        receive_events();
    } catch (const std::exception& e) {
        fail(std::string("Internal error: ") + e.what());
    }

    // Tell disconnect() that the connection has been read to its end.
    {
        std::lock_guard<std::mutex> lk(done_mu_);
        receiver_done_ = true;
    }
    done_cv_.notify_all();
}

// Reads until the connection ends.  It deliberately does NOT stop when the
// user quits: after our QUIT it keeps reading (and showing what still arrives,
// such as the echo of our last message) until the server closes, see
// disconnect().  Only a failure, or the shutdown() calls in disconnect(), end it.
void Client::receive_events() {
    while (true) {
        MsgType     type;
        std::string payload;
        FrameStatus st = recv_frame(sock_.fd(), type, payload);
        const int   err = errno;   // only meaningful for IO_ERROR

        if (st == FrameStatus::OK) {
            ChatEvent ev;
            if (make_event(type, payload, ev)) {
                sanitize_event(ev);                       // the server is not trusted
                if (ev.kind == EventKind::UserList) ev.timestamp = unix_now();
                screen_->show(ev);
                // From now on the end of the connection is the server's
                // announced shutdown, not a failure (see fail()).
                if (ev.kind == EventKind::System && ev.system_kind == SystemKind::Shutdown)
                    server_notified_.store(true);
                continue;
            }
            // A frame a client must never receive, or a malformed payload.
            st = FrameStatus::PROTOCOL_VIOLATION;
        }

        // Any failure ends the session.  fail() tells us who got there
        // first: if the main thread was already quitting on purpose
        // (/quit, Ctrl+D) the connection ending is just the server answering
        // our QUIT, so there is nothing to report and the exit code stays 0.
        fail("Connection lost (" + describe_failure(st, err) + ")");
        return;
    }
}

// ── Helpers used by the main thread ───────────────────────────────────────────
void Client::fail(const std::string& message) {
    // The first to flip running_ owns the story: it sets the exit code and the
    // reason.  run() prints the reason once the terminal is back to normal.
    //
    // A server that announced its shutdown (SYSTEM kind=shutdown) and then
    // closed the connection did nothing wrong: whoever notices the end first,
    // the outcome is "Server shut down" with exit code 0.  Without that
    // notice, the same end is a lost connection: exit code 1.
    const bool announced = server_notified_.load();
    if (running_.exchange(false)) {
        exit_code_.store(announced ? 0 : 1);
        end_kind_    = announced ? SystemKind::Shutdown : SystemKind::Error;
        end_message_ = announced ? "Server shut down" : message;
    }
}

// Splits the argument part of "/msg <user> <text>": the user is the first
// word, the text is everything after it with its leading whitespace removed.
// Returns false if either is missing.
static bool split_msg_args(const std::string& args, std::string& user, std::string& text) {
    constexpr const char* WS = " \t";
    const std::size_t user_start = args.find_first_not_of(WS);
    if (user_start == std::string::npos) return false;
    const std::size_t user_end = args.find_first_of(WS, user_start);
    if (user_end == std::string::npos) return false;
    const std::size_t text_start = args.find_first_not_of(WS, user_end);
    if (text_start == std::string::npos) return false;
    user = args.substr(user_start, user_end - user_start);
    text = args.substr(text_start);
    return true;
}

bool Client::send_private(const std::string& args) {
    std::string user, text;
    if (!split_msg_args(args, user, text)) {
        notice(SystemKind::Error, "Usage: /msg <user> <text>");
        return true;
    }
    // Same checks the server makes, so a typo costs no round trip.
    const NameCheck check = check_username(user);
    if (check != NameCheck::Ok) {
        notice(SystemKind::Error, std::string("Invalid username: ") + name_problem(check));
        return true;
    }
    if (text.size() > MAX_CHAT_TEXT) {
        notice(SystemKind::Error, "Message too long (max 1024 bytes), not sent");
        return true;
    }
    if (send_frame(sock_.fd(), MsgType::PRIV_SAY, encode_priv_say(user, text)) != FrameStatus::OK) {
        fail(std::string("Send failed: ") + std::strerror(errno));
        return false;
    }
    return true;       // the confirmation comes back through the receiver thread
}

bool Client::run_command(const std::string& line) {
    // The command is the first word; the rest (if any) is its arguments.
    const std::size_t cmd_end = line.find_first_of(" \t");
    const std::string cmd  = line.substr(0, cmd_end);
    const std::string args = (cmd_end == std::string::npos) ? "" : line.substr(cmd_end);

    if (cmd == "/help") {
        screen_->show_help();
        return true;
    }
    if (cmd == "/msg")
        return send_private(args);
    if (cmd == "/users") {
        if (send_frame(sock_.fd(), MsgType::LIST_REQ, "") != FrameStatus::OK) {
            fail(std::string("Send failed: ") + std::strerror(errno));
            return false;
        }
        return true;                 // the answer arrives through the receiver thread
    }
    // Plain mode has no screen to clear, so there /clear stays an unknown command.
    if (cmd == "/clear" && screen_->interactive()) {
        screen_->clear();
        return true;
    }
    if (cmd == "/quit")
        return false;                // run() sends QUIT and shuts the connection down

    notice(SystemKind::Error, "Unknown command: " + cmd + " (try /help)");
    return true;
}

bool Client::send_line(std::string line) {
    // Strip a trailing '\r' so the client works correctly when stdin comes
    // from a Windows-style CRLF source (e.g., a pipe).
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) return true;           // never send empty messages

    // A line starting with '/' is a command and is NEVER sent as chat.
    if (line.front() == '/') return run_command(line);

    if (line.size() > MAX_CHAT_TEXT) {
        notice(SystemKind::Error, "Message too long (max 1024 bytes), not sent");
        return true;                         // connection is still fine
    }

    if (send_frame(sock_.fd(), MsgType::SAY, line) != FrameStatus::OK) {
        fail(std::string("Send failed: ") + std::strerror(errno));
        return false;
    }
    return true;
}

// ── Main loop: stdin → send ───────────────────────────────────────────────────
enum class StdinStatus { Data, Idle, Interrupted, Eof, Failed };

// Waits up to 'timeout_ms' for stdin, then reads what is there.
//
// poll() + read() instead of std::cin/getline: stream buffering can hide data
// from poll() (the bytes sit in the C++ library buffer but poll() says
// "nothing to read").  The timeout lets the caller check 'running_' promptly
// when the server goes away.
//
// When the writer of a pipe/FIFO closes it, poll() reports POLLHUP (not POLLIN)
// once the data is drained.  A terminal reports POLLIN.  So all three count as
// "readable": read() then returns 0 on EOF, which is how input ending is found.
static StdinStatus read_stdin(char* buf, std::size_t cap, int timeout_ms,
                              ssize_t& n, std::string& error) {
    pollfd pfd{};
    pfd.fd     = STDIN_FILENO;
    pfd.events = POLLIN;

    const int ret = ::poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
        if (errno == EINTR) return StdinStatus::Interrupted;
        error = std::string("poll() on stdin failed: ") + std::strerror(errno);
        return StdinStatus::Failed;
    }
    if (ret == 0 || !(pfd.revents & (POLLIN | POLLHUP | POLLERR))) return StdinStatus::Idle;

    n = ::read(STDIN_FILENO, buf, cap);    // may hold several lines or half a line
    if (n < 0) {
        if (errno == EINTR) return StdinStatus::Interrupted;
        error = std::string("read() on stdin failed: ") + std::strerror(errno);
        return StdinStatus::Failed;
    }
    return n == 0 ? StdinStatus::Eof : StdinStatus::Data;   // EOF: Ctrl+D, or the pipe writer closed
}

// How long to wait for the rest of an escape sequence before a lone ESC is
// taken as the Escape key, and how long to sleep when nothing is going on.
static constexpr int ESC_WAIT_MS = 50;
static constexpr int IDLE_WAIT_MS = 200;

// Plain mode: cut the bytes into lines at '\n'; 'line_buf' keeps a half line
// across reads.
bool Client::handle_plain(const char* data, std::size_t n, std::string& line_buf) {
    for (std::size_t i = 0; i < n; ++i) {
        if (data[i] != '\n') { line_buf += data[i]; continue; }
        const bool ok = send_line(std::move(line_buf));
        line_buf.clear();
        if (!ok) return false;
    }
    return true;
}

// Interactive mode: feed the keys to the editor; every finished line is sent.
bool Client::handle_keys(const std::vector<Key>& keys) {
    for (const Key& key : keys) {
        std::string line;
        if (screen_->apply_key(key, line) && !send_line(std::move(line))) return false;
    }
    return true;
}

// Ctrl+C, SIGTERM and SIGHUP only set a flag (see SignalGuard); here the main
// loop turns it into an ordinary user quit, in plain mode too.  The handlers
// have no SA_RESTART, so a blocked poll() returns at once; and if the signal
// lands just before poll(), the 200 ms timeout bounds the delay.  In
// interactive mode this also picks up window resizes.
bool Client::service_terminal() {
    if (SignalGuard::stop_requested()) return false;
    if (screen_->interactive()) screen_->refresh();
    return true;
}

void Client::input_loop() {
    const bool ui = screen_->interactive();
    std::string line_buf;      // plain mode: text of a line that is not finished yet
    char buf[256];

    while (running_.load()) {
        if (!service_terminal()) return;

        ssize_t n = 0;
        std::string error;
        const int wait_ms = decoder_.pending() ? ESC_WAIT_MS : IDLE_WAIT_MS;
        switch (read_stdin(buf, sizeof(buf), wait_ms, n, error)) {
        case StdinStatus::Interrupted:
            break;
        case StdinStatus::Idle:
            // Nothing followed an ESC in time: it was the Escape key itself.
            if (ui && decoder_.pending() && !handle_keys(decoder_.flush())) return;
            break;
        case StdinStatus::Failed:
            fail(error);
            return;
        case StdinStatus::Eof:
            // Send the last unterminated line if there is one.
            if (!ui && running_.load() && !line_buf.empty()) send_line(std::move(line_buf));
            return;
        case StdinStatus::Data:
            if (ui ? !handle_keys(decoder_.feed(buf, static_cast<std::size_t>(n)))
                   : !handle_plain(buf, static_cast<std::size_t>(n), line_buf))
                return;
            break;
        }
    }
}

// How long a quitting client waits for the server to close its side.
static constexpr std::chrono::seconds QUIT_DRAIN_TIMEOUT{2};

void Client::disconnect() {
    // Claim the "who ended the session" decision.  If exchange() returns true
    // the user quit (/quit, Ctrl+D, Ctrl+C, SIGTERM): say goodbye properly so
    // the server can tell this apart from a crash.  If false, the receiver (or
    // fail()) already recorded the outcome and set the exit code.
    if (running_.exchange(false)) {
        send_frame(sock_.fd(), MsgType::QUIT, "");

        // Half-close: SHUT_WR sends OUR FIN after the QUIT, but the socket stays
        // readable.  Then keep reading until the server closes its side (the
        // receiver thread does the reading and flags it when it hits EOF) or
        // QUIT_DRAIN_TIMEOUT has passed.  Why not simply close()?  Closing a
        // socket that still has unread data in its receive queue makes the
        // kernel send a RST instead of a FIN.  A RST aborts the connection, so
        // the server may lose our final frames (the QUIT itself) and see a
        // "connection reset" instead of a clean leave.  Reading everything
        // first means the socket closes with an empty queue and a normal FIN.
        ::shutdown(sock_.fd(), SHUT_WR);
        std::unique_lock<std::mutex> lk(done_mu_);
        done_cv_.wait_for(lk, QUIT_DRAIN_TIMEOUT, [this] { return receiver_done_; });
    }

    // Wakes the receiver if it is still blocked in recv(): the drain timed out,
    // or the session failed on our side (a send error) while the receiver was
    // still waiting.  Harmless on a connection that is already finished.
    ::shutdown(sock_.fd(), SHUT_RDWR);

    if (recv_thread_.joinable()) recv_thread_.join();
}

int Client::run() {
    if (!join()) return 1;

    {
        // The UI (raw mode, banner, prompt) lives exactly as long as this block.
        ScreenSession session(*screen_, username_, host_, port_);
        recv_thread_ = std::thread(&Client::receiver_loop, this);
        input_loop();
        disconnect();
    }   // terminal restored here, on every way out

    // Only now, in normal terminal mode, say how the session ended: "Server
    // shut down" (exit 0) or why it failed (exit 1).  A user quit says nothing.
    if (!end_message_.empty()) notice(end_kind_, end_message_);
    return exit_code_.load();
}
