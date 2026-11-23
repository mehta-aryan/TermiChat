// client.cpp – Client implementation.

#include "client.hpp"
#include "text_utils.hpp"

#include <sys/socket.h>   // socket, connect, shutdown
#include <netdb.h>        // getaddrinfo, freeaddrinfo
#include <unistd.h>       // read, close
#include <poll.h>         // poll

#include <cstring>        // strerror, memset
#include <cstdio>         // fprintf, printf
#include <cerrno>
#include <iostream>
#include <mutex>
#include <string>

// ── Output ────────────────────────────────────────────────────────────────────
// show() is the ONLY function that turns a ChatEvent into terminal output, so
// restyling the UI means changing this one function.  The mutex keeps a line
// from the receiver thread and a local notice from the main thread apart.
static std::mutex g_out_mutex;

static void show(const ChatEvent& ev) {
    std::string line;
    switch (ev.kind) {
    case EventKind::Message:
        line = "[" + format_clock(ev.timestamp) + "] " + ev.sender + ": " + ev.text;
        break;
    case EventKind::Private:
        line = "[" + format_clock(ev.timestamp) + "] [private] " + ev.sender + " -> "
             + ev.recipient + ": " + ev.text;
        break;
    case EventKind::System:
        line = "*** " + ev.text;
        break;
    case EventKind::UserList:
        line = "*** Online (" + std::to_string(ev.users.size()) + "): "
             + join_strings(ev.users, ", ");
        break;
    }
    std::lock_guard<std::mutex> lk(g_out_mutex);
    std::cout << line << '\n';
    std::cout.flush();
}

// A message from the client itself (help, errors) rendered like a server notice.
static void notice(SystemKind kind, const std::string& text) {
    ChatEvent ev;
    ev.kind        = EventKind::System;
    ev.timestamp   = unix_now();
    ev.system_kind = kind;
    ev.text        = text;
    show(ev);
}

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

    std::cout << "Connected to " << host << ":" << port << '\n';
    std::cout.flush();
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
    while (running_.load()) {
        MsgType     type;
        std::string payload;
        FrameStatus st = recv_frame(sock_.fd(), type, payload);
        const int   err = errno;   // only meaningful for IO_ERROR

        if (st == FrameStatus::OK) {
            ChatEvent ev;
            if (make_event(type, payload, ev)) {
                show(ev);
                continue;
            }
            // A frame a client must never receive, or a malformed payload.
            st = FrameStatus::PROTOCOL_VIOLATION;
        }

        // Any failure ends the session.  exchange() tells us who got there
        // first: if running_ was already false, the main thread is quitting on
        // purpose (/quit, Ctrl+D) and its shutdown() is what woke us up, so
        // there is nothing to report and the exit code stays 0.
        if (!running_.exchange(false)) return;
        exit_code_.store(1);
        notice(SystemKind::Error, describe_failure(st, err));
    }
}

// ── Helpers used by the main thread ───────────────────────────────────────────
void Client::fail(const std::string& message) {
    // Same "who got there first" rule as the receiver: report only once.
    if (running_.exchange(false)) {
        exit_code_.store(1);
        notice(SystemKind::Error, message);
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
        notice(SystemKind::Info, "Commands:");
        notice(SystemKind::Info, "  /help               show this help");
        notice(SystemKind::Info, "  /users              list who is online");
        notice(SystemKind::Info, "  /msg <user> <text>  send a private message to one user");
        notice(SystemKind::Info, "  /quit               leave the chat (Ctrl+D works too)");
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
int Client::run() {
    if (!join()) return 1;

    // Start the receiver thread before entering the stdin loop.
    recv_thread_ = std::thread(&Client::receiver_loop, this);

    // We use poll() + read() on STDIN_FILENO instead of std::cin/getline
    // because stream buffering can hide data from poll() – the bytes sit in
    // the C++ library buffer but poll() says "nothing to read".
    // poll() with a 200 ms timeout lets us check 'running' promptly when the
    // server goes away.

    std::string line_buf;   // accumulates characters across partial reads
    char read_buf[256];
    bool stdin_eof = false;

    while (!stdin_eof && running_.load()) {
        pollfd pfd{};
        pfd.fd     = STDIN_FILENO;
        pfd.events = POLLIN;

        int ret = ::poll(&pfd, 1, 200);  // 200 ms timeout
        if (ret < 0) {
            if (errno == EINTR) continue;
            fail(std::string("poll() on stdin failed: ") + std::strerror(errno));
            break;
        }
        if (ret == 0) continue; // timeout – loop back to check running_

        // When the writer of a pipe/FIFO closes it, poll() reports POLLHUP
        // (not POLLIN) once the data is drained.  A real terminal's Ctrl+D
        // reports POLLIN.  Fall through to read() for all of them: it returns
        // 0 on EOF, which is how we detect that input has ended.
        if (!(pfd.revents & (POLLIN | POLLHUP | POLLERR))) continue;

        // read() may give us multiple lines or a partial line in one call.
        ssize_t n = ::read(STDIN_FILENO, read_buf, sizeof(read_buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            fail(std::string("read() on stdin failed: ") + std::strerror(errno));
            break;
        }
        if (n == 0) {          // EOF (Ctrl+D, or the pipe writer closed)
            stdin_eof = true;
            break;
        }

        bool ok = true;
        for (ssize_t i = 0; i < n && ok; ++i) {
            if (read_buf[i] == '\n') {
                ok = send_line(std::move(line_buf));
                line_buf.clear();
            } else {
                line_buf += read_buf[i];
            }
        }
        if (!ok) break;
    }

    // Send the last unterminated line at EOF if it is non-empty.
    if (stdin_eof && running_.load() && !line_buf.empty())
        send_line(std::move(line_buf));

    // Claim the "who ended the session" decision.  If exchange() returns true
    // the user quit (/quit or Ctrl+D): say goodbye properly so the server can
    // tell this apart from a crash.  If false, the receiver (or fail()) already
    // reported a problem and set the exit code.
    if (running_.exchange(false))
        send_frame(sock_.fd(), MsgType::QUIT, "");

    // shutdown() sends a TCP FIN to the server and unblocks the receiver
    // thread's recv() call, which then sees running_ == false and exits quietly.
    ::shutdown(sock_.fd(), SHUT_RDWR);

    if (recv_thread_.joinable()) recv_thread_.join();

    return exit_code_.load();
}
