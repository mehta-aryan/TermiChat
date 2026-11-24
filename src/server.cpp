// server.cpp – Server and ClientSession implementation.

#include "server.hpp"
#include "text_utils.hpp"

#include <arpa/inet.h>    // inet_ntop, htons, htonl, INADDR_ANY
#include <fcntl.h>        // open
#include <ifaddrs.h>      // getifaddrs, freeifaddrs
#include <net/if.h>       // IFF_UP, IFF_LOOPBACK
#include <netinet/in.h>   // sockaddr_in
#include <poll.h>         // poll
#include <sys/socket.h>   // socket, setsockopt, bind, listen, accept, shutdown
#include <sys/time.h>     // timeval
#include <unistd.h>       // close, isatty, _exit

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>        // getenv, strtol
#include <cstring>        // strerror
#include <ctime>          // localtime_r, strftime
#include <exception>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// ── Logging ──────────────────────────────────────────────────────────────────
// One mutex serialises all log output so lines from different threads do not
// interleave.  Flushing on every line ensures tests that redirect stdout see
// complete lines promptly.  It is the innermost lock (see server.hpp).
static std::mutex g_log_mutex;

enum class Level { Info, Warn, Error };

// Colour only on a terminal, and never when NO_COLOR is set (no-color.org).
static bool colour_enabled() {
    static const bool enabled = [] {
        const char* no_color = std::getenv("NO_COLOR");
        return ::isatty(STDOUT_FILENO) != 0 && !(no_color != nullptr && no_color[0] != '\0');
    }();
    return enabled;
}

// Format: "HH:MM:SS  LEVEL  message", the level tag coloured on a terminal.
static void log(Level level, const std::string& msg) {
    char tbuf[16];
    std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_buf);

    const char* tag   = "INFO ";
    const char* codes = "36";                       // cyan
    switch (level) {
    case Level::Info:                              break;
    case Level::Warn:  tag = "WARN ";  codes = "33"; break;    // yellow
    case Level::Error: tag = "ERROR";  codes = "31"; break;    // red
    }

    std::string line = tbuf;
    line += "  ";
    if (colour_enabled()) line += std::string("\x1b[") + codes + "m" + tag + "\x1b[0m";
    else                  line += tag;
    line += "  ";
    line += msg;
    line += '\n';

    std::lock_guard<std::mutex> lk(g_log_mutex);   // the line is built outside the lock
    std::cout << line;
    std::cout.flush();
}

// ── Console banner ───────────────────────────────────────────────────────────
// The machine's non-loopback IPv4 addresses on interfaces that are up.
static std::vector<std::string> lan_addresses() {
    std::vector<std::string> out;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return out;
    const std::unique_ptr<ifaddrs, decltype(&::freeifaddrs)> guard(list, ::freeifaddrs);

    for (const ifaddrs* ifa = list; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
        char buf[INET_ADDRSTRLEN];
        const auto* sin = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
        if (::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) != nullptr)
            out.emplace_back(buf);
    }
    return out;
}

// Only on a terminal: pipes and log files get log lines and nothing else.
static void print_banner(uint16_t port) {
    if (!::isatty(STDOUT_FILENO)) return;

    const bool colour = colour_enabled();
    const auto paint = [colour](const char* codes, const std::string& text) {
        return colour ? std::string("\x1b[") + codes + "m" + text + "\x1b[0m" : text;
    };

    const std::string title = "TermiChat Server";
    const std::string label = "Clients can connect to: ";
    const std::string indent(2 + label.size(), ' ');
    const std::string port_str = std::to_string(port);

    std::string out = "\n  " + paint("1;36", title)
                    + "\n  " + paint("2", std::string(title.size(), '-'))
                    + "\n  Port: " + paint("1", port_str) + "\n\n";

    const std::vector<std::string> addrs = lan_addresses();
    if (addrs.empty())
        out += "  " + label + paint("1", "localhost:" + port_str)
             + "  (no network interface is up)\n";
    for (std::size_t i = 0; i < addrs.size(); ++i)
        out += (i == 0 ? "  " + label : indent) + paint("1", addrs[i] + ":" + port_str) + "\n";
    out += "\n  Press Ctrl+C to stop\n\n";

    std::lock_guard<std::mutex> lk(g_log_mutex);
    std::cout << out;
    std::cout.flush();
}

static std::string format_uptime(std::chrono::seconds total) {
    const long long s = total.count();
    const long long h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    if (h > 0) return std::to_string(h) + "h " + std::to_string(m) + "m " + std::to_string(sec) + "s";
    if (m > 0) return std::to_string(m) + "m " + std::to_string(sec) + "s";
    return std::to_string(sec) + "s";
}

// ── Signals ──────────────────────────────────────────────────────────────────
// A handler may only do async-signal-safe things.  This one stores the signal
// number in a lock-free atomic that the accept loop polls.  The FIRST signal
// asks for a graceful stop; a SECOND one (an impatient Ctrl+C while shutting
// down) leaves at once with the conventional 128+signo status.
static std::atomic<int> g_stop_signal{0};
static_assert(std::atomic<int>::is_always_lock_free, "needed inside signal handlers");

static void on_stop_signal(int sig) {
    int expected = 0;
    if (!g_stop_signal.compare_exchange_strong(expected, sig)) ::_exit(128 + sig);
}

// Installs the handlers for the lifetime of the object and puts the previous
// ones back afterwards.
class SignalSetup {
public:
    SignalSetup() {
        install(SIGINT,  on_stop_signal, old_int_);
        install(SIGTERM, on_stop_signal, old_term_);
        // A write to a vanished peer must surface as EPIPE from send(), not kill
        // the server (MSG_NOSIGNAL on every send stays as the second safeguard).
        install(SIGPIPE, SIG_IGN, old_pipe_);
    }
    ~SignalSetup() {
        ::sigaction(SIGINT,  &old_int_,  nullptr);
        ::sigaction(SIGTERM, &old_term_, nullptr);
        ::sigaction(SIGPIPE, &old_pipe_, nullptr);
    }
    SignalSetup(const SignalSetup&)            = delete;
    SignalSetup& operator=(const SignalSetup&) = delete;

private:
    struct sigaction old_int_{}, old_term_{}, old_pipe_{};

    static void install(int sig, void (*handler)(int), struct sigaction& old) {
        struct sigaction sa{};
        sa.sa_handler = handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;        // no SA_RESTART: poll() must return EINTR when a signal lands
        if (::sigaction(sig, &sa, &old) != 0) throw NetError("sigaction");
    }
};

// ── Limits and timeouts ──────────────────────────────────────────────────────
// Test-only environment overrides (positive integers; anything else is ignored
// with a warning).  A normal run never sets them:
//   TERMICHAT_MAX_CLIENTS          connection limit, joined or not   (default 64)
//   TERMICHAT_HANDSHAKE_TIMEOUT_S  seconds of silence allowed before JOIN
//                                  completes                         (default 10)
//   TERMICHAT_SEND_TIMEOUT_S       longest a send() to one client may block
//                                                                    (default 5)
static constexpr int DEFAULT_MAX_CLIENTS          = 64;
static constexpr int DEFAULT_HANDSHAKE_TIMEOUT_S  = 10;
static constexpr int DEFAULT_SEND_TIMEOUT_S       = 5;

static int env_positive(const char* name, int fallback, int ceiling) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') return fallback;
    char* end = nullptr;
    const long value = std::strtol(raw, &end, 10);
    if (*end != '\0' || value < 1 || value > ceiling) {
        log(Level::Warn, std::string("Ignoring invalid ") + name + "='" + sanitize_text(raw)
                         + "' (expected 1-" + std::to_string(ceiling) + ")");
        return fallback;
    }
    return static_cast<int>(value);
}

// 0 seconds clears the timeout (the kernel's "wait for ever").
static bool set_timeout(int fd, int option, int seconds) {
    timeval tv{};
    tv.tv_sec = seconds;
    return ::setsockopt(fd, SOL_SOCKET, option, &tv, sizeof(tv)) == 0;
}

// ── ClientSession ─────────────────────────────────────────────────────────────
ClientSession::ClientSession(Socket sock, std::string peer_addr)
    : sock_(std::move(sock)), peer_addr_(std::move(peer_addr)) {}

FrameStatus ClientSession::send_frame(MsgType type, const std::string& payload) {
    std::lock_guard<std::timed_mutex> lk(send_mutex_);
    return ::send_frame(sock_.fd(), type, payload);
}

ClientSession::OutputLock ClientSession::lock_output() {
    return OutputLock(send_mutex_);
}

FrameStatus ClientSession::send_frame([[maybe_unused]] const OutputLock& held,
                                      MsgType type, const std::string& payload) {
    assert(held.owns_lock() && held.mutex() == &send_mutex_);
    return ::send_frame(sock_.fd(), type, payload);
}

FrameStatus ClientSession::try_send_frame(MsgType type, const std::string& payload,
                                          std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::timed_mutex> lk(send_mutex_, deadline);
    if (!lk.owns_lock()) return FrameStatus::IO_ERROR;      // someone is stuck sending to it
    return ::send_frame(sock_.fd(), type, payload, MSG_DONTWAIT);
}

// ── Registry ──────────────────────────────────────────────────────────────────
Server::Server(uint16_t port)
    : port_(port),
      limits_{static_cast<std::size_t>(env_positive("TERMICHAT_MAX_CLIENTS", DEFAULT_MAX_CLIENTS, 1024)),
              env_positive("TERMICHAT_HANDSHAKE_TIMEOUT_S", DEFAULT_HANDSHAKE_TIMEOUT_S, 3600),
              env_positive("TERMICHAT_SEND_TIMEOUT_S", DEFAULT_SEND_TIMEOUT_S, 3600)},
      started_(std::chrono::steady_clock::now()) {}

Server::~Server() { close_connections(); }

static std::vector<SessionPtr> sessions_of(const std::map<std::string, SessionPtr>& registry) {
    std::vector<SessionPtr> out;
    out.reserve(registry.size());
    for (const auto& entry : registry) out.push_back(entry.second);
    return out;
}

std::optional<std::vector<SessionPtr>>
Server::register_client(const SessionPtr& session, const std::string& name) {
    // Check and insert in ONE critical section: if two clients ask for the same
    // name at the same moment, exactly one try_emplace succeeds.
    std::lock_guard<std::mutex> lk(registry_mutex_);
    if (!clients_.try_emplace(to_lower(name), session).second) return std::nullopt;
    session->set_name(name);          // before any other thread can see the session
    return sessions_of(clients_);
}

std::vector<SessionPtr> Server::unregister_client(const SessionPtr& session) {
    std::lock_guard<std::mutex> lk(registry_mutex_);
    const auto it = clients_.find(to_lower(session->name()));
    if (it != clients_.end() && it->second == session) clients_.erase(it);
    return sessions_of(clients_);
}

std::vector<SessionPtr> Server::snapshot() {
    std::lock_guard<std::mutex> lk(registry_mutex_);
    return sessions_of(clients_);
}

SessionPtr Server::find_user(const std::string& name) {
    std::lock_guard<std::mutex> lk(registry_mutex_);
    const auto it = clients_.find(to_lower(name));
    return it == clients_.end() ? nullptr : it->second;     // copy out, then unlock
}

// ── Sending ───────────────────────────────────────────────────────────────────
void Server::report_send_failure(const ClientSession& session, FrameStatus status, int err) {
    // Silent when the client is already on its way out: it quit (or the server
    // is stopping) a moment before this broadcast reached it.  Nothing is wrong.
    if (!session.closing() && !stopping_.load()) {
        const std::string who = session.joined() ? session.name() : session.peer_addr();
        std::string why;
        if (status != FrameStatus::IO_ERROR)              why = "frame too large to send";
        else if (err == EAGAIN || err == EWOULDBLOCK)     why = "send timeout";   // SO_SNDTIMEO expired
        else                                              why = std::strerror(err);
        log(Level::Warn, "Send to " + who + " failed: " + why + " – shutting down its socket");
    }
    // shutdown() unblocks the client's own recv() loop so that thread will
    // clean up.  We do NOT close the fd here because we don't own it – closing
    // another thread's fd would create a race condition.
    ::shutdown(session.fd(), SHUT_RDWR);
}

bool Server::deliver(const SessionPtr& session, MsgType type, const std::string& payload) {
    const FrameStatus st = session->send_frame(type, payload);
    if (st == FrameStatus::OK) return true;
    const int err = errno;   // capture before anything else can change it
    report_send_failure(*session, st, err);
    return false;
}

void Server::send_error(const SessionPtr& session, const std::string& text) {
    deliver(session, MsgType::SYSTEM, encode_system(unix_now(), SystemKind::Error, text));
}

void Server::broadcast(const std::vector<SessionPtr>& targets, MsgType type,
                       const std::string& payload) {
    for (const SessionPtr& session : targets) deliver(session, type, payload);
}

// ── Per-client thread ─────────────────────────────────────────────────────────
Server::Disconnect Server::reject_join(const SessionPtr& session, const std::string& reason) {
    // Best effort: the connection is closed right after, whatever happens here.
    session->send_frame(MsgType::JOIN_ERR, reason);
    return Disconnect{false, reason};
}

std::optional<Server::Disconnect>
Server::handle_join(const SessionPtr& session, const std::string& name) {
    const NameCheck check = check_username(name);
    if (check != NameCheck::Ok) return reject_join(session, name_problem(check));

    // The handshake is over: lift the receive timeout so that a joined user may
    // stay silent for as long as they like.  Done before the name is claimed, so
    // a failure here leaves nothing to undo.
    if (!set_timeout(session->fd(), SO_RCVTIMEO, 0))
        return reject_join(session, "Server error");

    // Hold this session's output lock from before it becomes visible in the
    // registry until JOIN_OK and the welcome line are written.  Another thread
    // that broadcasts to the new user in between just waits, so JOIN_OK is
    // always the first frame the client sees (it waits for exactly that).
    ClientSession::OutputLock out = session->lock_output();
    const std::optional<std::vector<SessionPtr>> roster = register_client(session, name);
    if (!roster) {
        out.unlock();
        return reject_join(session, "Username already taken");
    }

    std::vector<std::string> names;
    std::vector<SessionPtr>  others;        // everyone who was already online
    for (const SessionPtr& s : *roster) {
        names.push_back(s->name());
        if (s != session) others.push_back(s);
    }

    std::size_t peak = peak_users_.load();
    while (names.size() > peak && !peak_users_.compare_exchange_weak(peak, names.size())) {}

    const std::string welcome = "Welcome to TermiChat, " + name + "! Online ("
                              + std::to_string(names.size()) + "): " + join_strings(names, ", ");
    const std::pair<MsgType, std::string> greeting[] = {
        {MsgType::JOIN_OK, name},
        {MsgType::SYSTEM,  encode_system(unix_now(), SystemKind::Info, welcome)},
    };
    for (const auto& [type, payload] : greeting) {
        const FrameStatus st = session->send_frame(out, type, payload);
        if (st != FrameStatus::OK) {
            const int err = errno;
            report_send_failure(*session, st, err);
            break;      // its thread sees the shutdown and runs the normal leave path
        }
    }
    out.unlock();

    // The joiner is now fully set up; tell the others.  'others' was taken in
    // the same critical section as the insert, so nobody hears about this user
    // twice (once in a welcome list and once as a notice) or not at all.
    broadcast(others, MsgType::SYSTEM,
              encode_system(unix_now(), SystemKind::Join, name + " joined the chat"));
    log(Level::Info, name + " joined from " + session->peer_addr()
        + " (" + std::to_string(names.size()) + " online)");
    return std::nullopt;
}

std::optional<Server::Disconnect>
Server::handle_say(const SessionPtr& session, const std::string& payload) {
    // recv_frame accepts up to MAX_PAYLOAD so the framing layer stays
    // generic, but a chat line is limited to MAX_CHAT_TEXT.  The limit is
    // checked on the RAW payload, before sanitising.
    if (payload.size() > MAX_CHAT_TEXT)
        return Disconnect{false, "protocol violation (chat text longer than "
                                 + std::to_string(MAX_CHAT_TEXT) + " bytes)"};

    const std::string text = sanitize_text(payload);
    if (text.empty()) return std::nullopt;       // nothing left to say

    // Sender and time come from the server, never from the client.  The sender
    // gets its own message back too: the UI shows it from the same transcript.
    // Sanitising never grows the text, so the frame is always <= MAX_PAYLOAD.
    broadcast(snapshot(), MsgType::MSG, encode_msg(unix_now(), session->name(), text));
    relayed_.fetch_add(1);
    return std::nullopt;
}

std::optional<Server::Disconnect>
Server::handle_priv_say(const SessionPtr& session, const std::string& payload) {
    std::string target, raw_text;
    if (!decode_priv_say(payload, target, raw_text))
        return Disconnect{false, "protocol violation (malformed PRIV_SAY)"};
    // Same limit as SAY, on the RAW text.  The client checks the target before
    // sending, so a bad one can only come from a hand-made client.
    if (raw_text.size() > MAX_CHAT_TEXT)
        return Disconnect{false, "protocol violation (private text longer than "
                                 + std::to_string(MAX_CHAT_TEXT) + " bytes)"};
    if (check_username(target) != NameCheck::Ok)
        return Disconnect{false, "protocol violation (invalid private-message target)"};

    const std::string text = sanitize_text(raw_text);
    if (text.empty()) {
        send_error(session, "Message is empty");
        return std::nullopt;
    }
    if (to_lower(target) == to_lower(session->name())) {
        send_error(session, "You can't send a private message to yourself");
        return std::nullopt;
    }
    const SessionPtr recipient = find_user(target);   // registry lock already released
    if (!recipient) {
        send_error(session, "No such user: " + target);   // target passed check_username: safe to echo
        return std::nullopt;
    }

    // Why two sends are safe: the registry lock is gone, so no lock is held
    // across I/O.  Each send takes only ITS OWN session's send_mutex, one after
    // the other and never both at once, so two users messaging each other at
    // the same moment cannot deadlock, and frames never interleave on a socket.
    // 'recipient' is a shared_ptr copy, so its fd cannot be closed or reused
    // while we write.  A relayed text is never longer than the raw text, so the
    // frame always fits MAX_PAYLOAD.  The sender's copy goes last: no
    // confirmation is shown for a message that was not delivered.
    const std::string frame = encode_priv_msg(unix_now(), session->name(), recipient->name(), text);
    if (!deliver(recipient, MsgType::PRIV_MSG, frame)) {
        send_error(session, recipient->name() + " is no longer online");
        return std::nullopt;
    }
    deliver(session, MsgType::PRIV_MSG, frame);
    relayed_.fetch_add(1);

    // Privacy: the log gets who and how much, never what.
    log(Level::Info, session->name() + " -> " + recipient->name()
        + " private message (" + std::to_string(text.size()) + " bytes)");
    return std::nullopt;
}

void Server::handle_list(const SessionPtr& session) {
    std::vector<std::string> names;
    for (const SessionPtr& s : snapshot()) names.push_back(s->name());
    deliver(session, MsgType::LIST_RES, encode_user_list(names));
}

std::optional<Server::Disconnect>
Server::handle_frame(const SessionPtr& session, MsgType type, const std::string& payload) {
    if (!session->joined()) {
        if (type != MsgType::JOIN) return Disconnect{false, "expected JOIN as the first frame"};
        return handle_join(session, payload);
    }

    switch (type) {
    case MsgType::SAY:
        return handle_say(session, payload);
    case MsgType::PRIV_SAY:
        return handle_priv_say(session, payload);
    case MsgType::LIST_REQ:
        if (!payload.empty()) return Disconnect{false, "protocol violation (LIST_REQ has a payload)"};
        handle_list(session);
        return std::nullopt;
    case MsgType::QUIT:
        if (!payload.empty()) return Disconnect{false, "protocol violation (QUIT has a payload)"};
        return Disconnect{true, "quit"};
    case MsgType::JOIN:
        return Disconnect{false, "protocol violation (JOIN sent twice)"};
    default:    // JOIN_OK, JOIN_ERR, MSG, PRIV_MSG, SYSTEM, LIST_RES only travel server -> client
        return Disconnect{false, "protocol violation (message type not allowed from a client)"};
    }
}

Server::Disconnect Server::serve_client(const SessionPtr& session) {
    while (true) {
        MsgType     type;
        std::string payload;
        FrameStatus st = recv_frame(session->fd(), type, payload);
        const int   err = errno;   // only meaningful for IO_ERROR

        switch (st) {
        case FrameStatus::OK:
            break;
        case FrameStatus::PEER_CLOSED:
            // A client that leaves politely sends QUIT first, so a bare FIN
            // means it crashed, was killed or lost its network.
            if (!session->joined()) return {false, "connection closed before JOIN"};
            return {false, "connection closed without QUIT"};
        case FrameStatus::PEER_CLOSED_MIDFRAME:
            return {false, "peer closed connection mid-frame (truncated frame)"};
        case FrameStatus::IO_ERROR:
            // SO_RCVTIMEO is only armed until JOIN completes, so an expired
            // receive timeout can only mean the handshake took too long.
            if (!session->joined() && (err == EAGAIN || err == EWOULDBLOCK))
                return {false, "handshake timeout"};
            return {false, std::string("recv error: ") + std::strerror(err)};
        case FrameStatus::PROTOCOL_VIOLATION:
            return {false, "protocol violation (bad length or unknown type)"};
        }

        if (std::optional<Disconnect> end = handle_frame(session, type, payload)) return *end;
    }
}

namespace {
// Raises a flag when the client thread returns, whichever way it leaves: the
// accept loop only joins threads that have raised it.
struct FinishedFlag {
    std::atomic<bool>* flag;
    ~FinishedFlag() { flag->store(true); }
};
}  // namespace

void Server::client_thread(SessionPtr session, std::atomic<bool>* finished) {
    const FinishedFlag done{finished};

    Disconnect why{false, "internal error"};
    try {
        why = serve_client(session);
    } catch (const std::exception& e) {
        why = {false, std::string("internal error: ") + e.what()};
    } catch (...) {
        why = {false, "internal error (unknown exception)"};
    }

    // From here on a failed send to this client is a lost race, not a problem.
    session->mark_closing();

    // Send our FIN now (a violating peer should not wait for the last
    // shared_ptr to drop).  The fd itself is closed by ~Socket when the last
    // reference disappears.
    ::shutdown(session->fd(), SHUT_RDWR);

    // Never joined: it was never online, so there is no leave notice.
    if (!session->joined()) {
        if (!stopping_.load()) log(Level::Warn, session->peer_addr() + " rejected: " + why.reason);
        return;
    }

    const std::vector<SessionPtr> remaining = unregister_client(session);

    // During shutdown every client is being disconnected: no per-user log
    // lines and no leave notices (their sockets are already shut down).  The
    // summary at the end says it all.
    if (stopping_.load()) return;

    log(why.clean ? Level::Info : Level::Warn,
        session->name() + " left (" + why.reason + ") (" + std::to_string(remaining.size()) + " online)");
    broadcast(remaining, MsgType::SYSTEM,
              encode_system(unix_now(), SystemKind::Leave,
                            session->name() + (why.clean ? " left the chat" : " lost connection")));
}

// ── Connection bookkeeping (accept thread) ───────────────────────────────────
std::size_t Server::live_connections() const {
    return static_cast<std::size_t>(std::count_if(
        connections_.begin(), connections_.end(),
        [](const Connection& c) { return !c.finished.load(); }));
}

void Server::reap_finished() {
    // Move the finished entries out of the list first, then join them.  A
    // thread raises 'finished' as its last act, so join() returns at once, and
    // an entry that has left the list can never be joined a second time.
    std::list<Connection> done;
    for (auto it = connections_.begin(); it != connections_.end();) {
        const auto next = std::next(it);
        if (it->finished.load()) done.splice(done.end(), connections_, it);
        it = next;
    }
    for (Connection& c : done) c.thread.join();
}   // 'done' dies here: the sockets of the finished sessions are closed

void Server::close_connections() {
    std::list<Connection> all;
    all.swap(connections_);            // nodes keep their addresses: the threads point at them

    // Every connection, joined or not: a thread that is blocked in recv() (or
    // in a send() to a stalled client) wakes up as soon as its socket is shut down.
    for (Connection& c : all) ::shutdown(c.session->fd(), SHUT_RDWR);
    for (Connection& c : all)
        if (c.thread.joinable()) c.thread.join();
}

static constexpr std::chrono::milliseconds SHUTDOWN_NOTICE_BUDGET{300};

void Server::announce_shutdown() {
    const std::string notice =
        encode_system(unix_now(), SystemKind::Shutdown, "Server is shutting down");

    // One shared deadline for ALL users: a client that does not read must not
    // be able to delay the notice to the others, or the shutdown itself, by even
    // a single send timeout.  try_send_frame() never waits for a stuck sender
    // and never blocks on a full buffer; whoever is skipped just sees the
    // connection close in the next step.
    const auto deadline = std::chrono::steady_clock::now() + SHUTDOWN_NOTICE_BUDGET;
    for (const SessionPtr& session : snapshot())
        session->try_send_frame(MsgType::SYSTEM, notice, deadline);
}

// ── accept() loop ─────────────────────────────────────────────────────────────
static constexpr int LISTEN_BACKLOG  = 16;
static constexpr int ACCEPT_POLL_MS  = 200;    // how often the loop looks at the stop flag
static constexpr int ACCEPT_BACKOFF_MS = 100;

void Server::accept_failed(int err, int listen_fd, Socket& spare) {
    if (err == EINTR || err == ECONNABORTED) return;       // transient

    if (err == EMFILE || err == ENFILE) {
        // Out of file descriptors.  The pending connection stays in the
        // kernel's queue, so poll() would report the listener readable again at
        // once and the loop would spin.  Give back the spare descriptor, accept
        // that one connection just to close it (its peer sees a refusal instead
        // of hanging in the queue), then take a new spare.
        spare = Socket(-1);
        { const Socket dropped(::accept(listen_fd, nullptr, nullptr)); }
        spare = Socket(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        log(Level::Error, "accept(): out of file descriptors – dropped one connection");
    } else {
        log(Level::Warn, "accept() error: " + std::string(std::strerror(err)));
    }
    // Resource exhaustion rarely clears in a microsecond: back off briefly
    // instead of spinning on a listener that stays readable.
    std::this_thread::sleep_for(std::chrono::milliseconds(ACCEPT_BACKOFF_MS));
}

void Server::accept_connection(int listen_fd, Socket& spare) {
    // accept() dequeues one connection the kernel has already finished the
    // three-way handshake for.  It returns a NEW fd for that connection;
    // listen_fd stays in the listening state.
    sockaddr_in peer_addr{};
    socklen_t   peer_len = sizeof(peer_addr);
    const int   cli_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
    if (cli_fd < 0) {
        accept_failed(errno, listen_fd, spare);
        return;
    }
    Socket sock(cli_fd);            // owned from here on: every return below closes it

    // Format "IP:port" string for logging using inet_ntop (thread-safe;
    // avoids the deprecated inet_ntoa which uses a static buffer).
    char ip_buf[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &peer_addr.sin_addr, ip_buf, sizeof(ip_buf));
    const std::string peer_str = std::string(ip_buf) + ":" + std::to_string(ntohs(peer_addr.sin_port));

    // Two timeouts keep a misbehaving client from holding a thread (or the
    // whole room) hostage:
    //
    //  - SO_RCVTIMEO until JOIN completes: a connection that never sends JOIN
    //    would otherwise occupy a thread and one of the limited connection
    //    slots for ever.  handle_join() clears it; a joined user may idle.
    //
    //  - SO_SNDTIMEO on every socket (backpressure).  Every chat message is
    //    relayed to every user, so one slow reader sets the pace of the whole
    //    room unless something bounds it.  When a client stops reading, its
    //    kernel receive buffer and our send buffer fill up and send() BLOCKS.
    //    The relaying thread sits in that send() holding the client's
    //    send_mutex_, so every other thread that wants to send to it queues up
    //    behind, and the broadcaster cannot reach the users after the stalled
    //    one in its list: the whole room freezes behind one stalled reader.
    //    With the timeout the blocked send fails with EAGAIN after a few
    //    seconds; only that client is dropped and everybody else carries on.
    //    The delay is bounded: at most one timeout, once per stalled client.
    if (!set_timeout(sock.fd(), SO_SNDTIMEO, limits_.send_timeout_s) ||
        !set_timeout(sock.fd(), SO_RCVTIMEO, limits_.handshake_timeout_s)) {
        log(Level::Warn, peer_str + " rejected: could not set socket timeouts: " + std::strerror(errno));
        return;
    }

    // The limit counts connections, joined or not: a flood of silent sockets
    // must not be able to use up threads and descriptors.  An extra client is
    // told why (best effort; the fresh send buffer is empty, so this cannot
    // block the accept loop) and dropped without a thread.
    if (live_connections() >= limits_.max_clients) {
        log(Level::Warn, peer_str + " rejected: server is full ("
            + std::to_string(limits_.max_clients) + " connections)");
        ::send_frame(sock.fd(), MsgType::JOIN_ERR, "Server is full");
        ::shutdown(sock.fd(), SHUT_RDWR);
        return;
    }

    // The session starts unjoined: it is NOT in the registry until its
    // JOIN is accepted, so it receives no broadcasts and is not "online".
    auto session = std::make_shared<ClientSession>(std::move(sock), peer_str);
    log(Level::Info, peer_str + " connected");

    // One tracked thread per client; reap_finished() / close_connections()
    // join it.  std::thread's constructor throws if the OS refuses a new thread
    // (resource limits); drop just this client and keep serving the others
    // rather than letting the exception kill us.
    connections_.emplace_back(session);
    Connection& conn = connections_.back();
    try {
        conn.thread = std::thread(&Server::client_thread, this, session, &conn.finished);
    } catch (const std::system_error& e) {
        connections_.pop_back();
        log(Level::Warn, peer_str + " rejected: could not start a thread: " + e.what());
    }
}

void Server::run() {
    // Handlers first, so a Ctrl+C during setup is a clean stop, not a kill.
    const SignalSetup signals;

    // socket() asks the kernel to create a TCP endpoint.
    // AF_INET = IPv4, SOCK_STREAM = TCP, 0 = default protocol.
    int srv_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) throw NetError("socket()");
    Socket srv_sock(srv_fd);

    // SO_REUSEADDR lets us restart the server while old connections from a
    // previous run are still in TIME_WAIT.  Without it, bind() would fail
    // with EADDRINUSE for up to 2 minutes after a restart.
    int opt = 1;
    if (::setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        throw NetError("setsockopt(SO_REUSEADDR)");

    // bind() assigns the local address (all interfaces, chosen port) to the
    // socket so that the kernel knows which incoming packets belong here.
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;      // accept on all interfaces
    addr.sin_port        = htons(port_);    // host byte order → network byte order
    if (::bind(srv_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw NetError("bind()");

    // listen() moves the socket into the passive state: the kernel will now
    // accept TCP SYN packets and complete the three-way handshake automatically,
    // queuing up to LISTEN_BACKLOG fully-connected sockets for accept() to pick.
    if (::listen(srv_fd, LISTEN_BACKLOG) < 0)
        throw NetError("listen()");

    // A descriptor held in reserve for the EMFILE case (see accept_failed).
    Socket spare(::open("/dev/null", O_RDONLY | O_CLOEXEC));

    print_banner(port_);
    log(Level::Info, "Listening on 0.0.0.0:" + std::to_string(port_));

    // poll() with a timeout instead of a blocking accept(): the loop wakes
    // every 200 ms to look at the stop flag, so a signal that was delivered to
    // some other thread (or that landed just before poll()) is still noticed
    // promptly.  A signal that does interrupt poll() gives EINTR: just loop.
    while (g_stop_signal.load() == 0) {
        pollfd pfd{};
        pfd.fd     = srv_fd;
        pfd.events = POLLIN;
        const int ready     = ::poll(&pfd, 1, ACCEPT_POLL_MS);
        const int poll_err  = errno;

        reap_finished();            // a long-running server must not pile up dead threads

        if (ready < 0) {
            if (poll_err == EINTR) continue;
            errno = poll_err;
            throw NetError("poll()");       // ~Server() still shuts the clients down
        }
        if (ready > 0) accept_connection(srv_fd, spare);
    }

    // ── Graceful shutdown ──
    const int sig = g_stop_signal.load();
    stopping_.store(true);              // client threads now stay quiet
    reap_finished();
    log(Level::Info, std::string("Received ") + (sig == SIGTERM ? "SIGTERM" : "SIGINT")
        + ", shutting down (" + std::to_string(live_connections()) + " connection(s) open)");

    srv_sock = Socket(-1);              // close the listening socket: no new clients
    announce_shutdown();
    close_connections();

    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started_);
    log(Level::Info, "Server stopped. Uptime " + format_uptime(uptime)
        + ", peak users " + std::to_string(peak_users_.load())
        + ", messages relayed " + std::to_string(relayed_.load()));
}
