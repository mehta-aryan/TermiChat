// server.cpp – Server and ClientSession implementation.

#include "server.hpp"
#include "text_utils.hpp"

#include <arpa/inet.h>    // inet_ntop, htons, htonl, INADDR_ANY
#include <netinet/in.h>   // sockaddr_in
#include <sys/socket.h>   // socket, setsockopt, bind, listen, accept, shutdown
#include <unistd.h>       // close

#include <thread>
#include <chrono>
#include <cassert>
#include <cstring>        // strerror
#include <ctime>          // localtime_r, strftime
#include <cerrno>
#include <exception>
#include <iostream>
#include <system_error>
#include <utility>
#include <vector>
#include <mutex>

// ── Logging ──────────────────────────────────────────────────────────────────
// One mutex serialises all log output so lines from different threads do not
// interleave.  Flushing on every line ensures tests that redirect stdout see
// complete lines promptly.
static std::mutex g_log_mutex;

static void log(const std::string& msg) {
    // Format: [HH:MM:SS] msg
    char tbuf[16];
    std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_buf);

    std::lock_guard<std::mutex> lk(g_log_mutex);
    std::cout << '[' << tbuf << "] " << msg << '\n';
    std::cout.flush();
}

// ── ClientSession ─────────────────────────────────────────────────────────────
ClientSession::ClientSession(Socket sock, std::string peer_addr)
    : sock_(std::move(sock)), peer_addr_(std::move(peer_addr)) {}

FrameStatus ClientSession::send_frame(MsgType type, const std::string& payload) {
    std::lock_guard<std::mutex> lk(send_mutex_);
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

// ── Registry ──────────────────────────────────────────────────────────────────
Server::Server(uint16_t port) : port_(port) {}

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

// ── Sending ───────────────────────────────────────────────────────────────────
void Server::report_send_failure(const ClientSession& session, FrameStatus status, int err) {
    const std::string who = session.joined() ? session.name() : session.peer_addr();
    const std::string why = (status == FrameStatus::IO_ERROR) ? std::strerror(err)
                                                              : "frame too large to send";
    log("[WARN] Send to " + who + " failed: " + why + " – shutting down its socket");
    // shutdown() unblocks the client's own recv() loop so that thread will
    // clean up.  We do NOT close the fd here because we don't own it – closing
    // another thread's fd would create a race condition.
    ::shutdown(session.fd(), SHUT_RDWR);
}

void Server::deliver(const SessionPtr& session, MsgType type, const std::string& payload) {
    const FrameStatus st = session->send_frame(type, payload);
    if (st != FrameStatus::OK) {
        const int err = errno;   // capture before anything else can change it
        report_send_failure(*session, st, err);
    }
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
    log("[INFO] " + name + " joined from " + session->peer_addr()
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
    case MsgType::LIST_REQ:
        if (!payload.empty()) return Disconnect{false, "protocol violation (LIST_REQ has a payload)"};
        handle_list(session);
        return std::nullopt;
    case MsgType::QUIT:
        if (!payload.empty()) return Disconnect{false, "protocol violation (QUIT has a payload)"};
        return Disconnect{true, "quit"};
    case MsgType::JOIN:
        return Disconnect{false, "protocol violation (JOIN sent twice)"};
    default:    // JOIN_OK, JOIN_ERR, MSG, SYSTEM, LIST_RES only travel server -> client
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
            return {false, std::string("recv error: ") + std::strerror(err)};
        case FrameStatus::PROTOCOL_VIOLATION:
            return {false, "protocol violation (bad length or unknown type)"};
        }

        if (std::optional<Disconnect> end = handle_frame(session, type, payload)) return *end;
    }
}

void Server::client_thread(SessionPtr session) {
    Disconnect why{false, "internal error"};
    try {
        why = serve_client(session);
    } catch (const std::exception& e) {
        why = {false, std::string("internal error: ") + e.what()};
    } catch (...) {
        why = {false, "internal error (unknown exception)"};
    }

    // Send our FIN now (a violating peer should not wait for the last
    // shared_ptr to drop).  The fd itself is closed by ~Socket when the last
    // reference disappears.
    ::shutdown(session->fd(), SHUT_RDWR);

    // Never joined: it was never online, so there is no leave notice.
    if (!session->joined()) {
        log("[WARN] " + session->peer_addr() + " rejected: " + why.reason);
        return;
    }

    const std::vector<SessionPtr> remaining = unregister_client(session);
    log(std::string(why.clean ? "[INFO] " : "[WARN] ") + session->name() + " left ("
        + why.reason + ") (" + std::to_string(remaining.size()) + " online)");
    broadcast(remaining, MsgType::SYSTEM,
              encode_system(unix_now(), SystemKind::Leave,
                            session->name() + (why.clean ? " left the chat" : " lost connection")));
}

// ── accept() loop ─────────────────────────────────────────────────────────────
static constexpr int LISTEN_BACKLOG = 16;

void Server::run() {
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

    log("[INFO] Listening on 0.0.0.0:" + std::to_string(port_));

    while (true) {
        // accept() dequeues one connection the kernel has already finished the
        // three-way handshake for.  It returns a NEW fd for that connection;
        // srv_fd stays in the listening state.
        sockaddr_in peer_addr{};
        socklen_t   peer_len = sizeof(peer_addr);
        int cli_fd = ::accept(srv_fd,
                              reinterpret_cast<sockaddr*>(&peer_addr),
                              &peer_len);

        if (cli_fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue; // transient
            // Resource exhaustion: log and sleep briefly before retrying.
            log("[WARN] accept() error: " + std::string(std::strerror(errno)));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Format "IP:port" string for logging using inet_ntop (thread-safe;
        // avoids the deprecated inet_ntoa which uses a static buffer).
        char ip_buf[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &peer_addr.sin_addr, ip_buf, sizeof(ip_buf));
        std::string peer_str = std::string(ip_buf) + ":"
                             + std::to_string(ntohs(peer_addr.sin_port));

        // The session starts unjoined: it is NOT in the registry until its
        // JOIN is accepted, so it receives no broadcasts and is not "online".
        auto session = std::make_shared<ClientSession>(Socket(cli_fd), peer_str);
        log("[INFO] " + peer_str + " connected");

        // Spawn a detached thread for this client's recv loop.
        // Phase 4 will replace detached threads with tracked, joined threads
        // for graceful shutdown.  std::thread's constructor throws if the OS
        // refuses a new thread (resource limits); drop just this client and
        // keep serving the others rather than letting the exception kill us.
        try {
            std::thread(&Server::client_thread, this, session).detach();
        } catch (const std::system_error& e) {
            log("[WARN] " + peer_str + " rejected: could not start a thread: "
                + e.what());
        }
    }
}
