// server.cpp – Server and ClientSession implementation.

#include "server.hpp"

#include <arpa/inet.h>    // inet_ntop, htons, htonl, INADDR_ANY
#include <netinet/in.h>   // sockaddr_in
#include <sys/socket.h>   // socket, setsockopt, bind, listen, accept, shutdown
#include <unistd.h>       // close

#include <thread>
#include <chrono>
#include <cstring>        // strerror
#include <ctime>          // localtime_r, strftime
#include <cstdio>         // snprintf
#include <cerrno>
#include <iostream>
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
ClientSession::ClientSession(int id, Socket sock, std::string peer_addr)
    : id_(id), sock_(std::move(sock)), peer_addr_(std::move(peer_addr)) {}

FrameStatus ClientSession::send_frame(MsgType type, const std::string& payload) {
    // The send_mutex ensures that only one thread at a time writes to this
    // socket, so frames from different broadcasters cannot be interleaved.
    std::lock_guard<std::mutex> lk(send_mutex_);
    return ::send_frame(sock_.fd(), type, payload);
}

// ── Server helpers ────────────────────────────────────────────────────────────
Server::Server(uint16_t port) : port_(port) {}

void Server::add_client(std::shared_ptr<ClientSession> session) {
    std::lock_guard<std::mutex> lk(registry_mutex_);
    clients_[session->id()] = std::move(session);
}

void Server::remove_client(int id) {
    std::lock_guard<std::mutex> lk(registry_mutex_);
    clients_.erase(id);
}

void Server::broadcast(int sender_id, MsgType type, const std::string& payload) {
    // Snapshot the session list under the lock then release it before I/O.
    // Holding the lock during send() would stall all other threads if one
    // recipient is slow or dead.
    std::vector<std::shared_ptr<ClientSession>> targets;
    {
        std::lock_guard<std::mutex> lk(registry_mutex_);
        for (auto& [id, sess] : clients_) {
            if (id != sender_id) targets.push_back(sess);
        }
    }

    for (auto& sess : targets) {
        FrameStatus st = sess->send_frame(type, payload);
        if (st != FrameStatus::OK) {
            log("[WARN] Send to Client " + std::to_string(sess->id())
                + " (" + sess->peer_addr() + ") failed: " + std::strerror(errno)
                + " – shutting down its socket");
            // shutdown() unblocks the client's own recv() loop so that thread
            // will clean up.  We do NOT close the fd here because we don't own
            // it – closing another thread's fd would create a race condition.
            ::shutdown(sess->fd(), SHUT_RDWR);
        }
    }
}

// ── Per-client thread ─────────────────────────────────────────────────────────
void Server::client_thread(std::shared_ptr<ClientSession> session) {
    // Wrap thread entry point in try/catch so no exception can reach std::terminate.
    try {
        const int    cid  = session->id();
        const std::string peer = session->peer_addr();

        // Count online clients for the log message.
        auto online_count = [this]() {
            std::lock_guard<std::mutex> lk(registry_mutex_);
            return clients_.size();
        };

        while (true) {
            MsgType     type;
            std::string payload;
            FrameStatus st = recv_frame(session->fd(), type, payload);

            if (st == FrameStatus::PEER_CLOSED) {
                remove_client(cid);
                log("[INFO] Client " + std::to_string(cid) + " disconnected"
                    " (peer closed connection) ("
                    + std::to_string(online_count()) + " online)");
                return;
            }
            if (st == FrameStatus::PEER_CLOSED_MIDFRAME) {
                remove_client(cid);
                log("[WARN] Client " + std::to_string(cid)
                    + " disconnected mid-frame (incomplete frame) ("
                    + std::to_string(online_count()) + " online)");
                return;
            }
            if (st == FrameStatus::IO_ERROR) {
                remove_client(cid);
                log("[WARN] Client " + std::to_string(cid)
                    + " I/O error: " + std::strerror(errno) + " ("
                    + std::to_string(online_count()) + " online)");
                return;
            }
            if (st == FrameStatus::PROTOCOL_VIOLATION) {
                remove_client(cid);
                log("[WARN] Client " + std::to_string(cid)
                    + " protocol violation – dropping ("
                    + std::to_string(online_count()) + " online)");
                ::shutdown(session->fd(), SHUT_RDWR);
                return;
            }

            // st == OK; type == CHAT (recv_frame already validated it).
            if (payload.empty()) continue; // skip empty messages

            // Build "[HH:MM] [Client N]: <text>" using localtime_r (thread-safe;
            // the POSIX version of localtime uses thread-local storage).
            char tbuf[8];
            std::time_t t = std::time(nullptr);
            std::tm tm_buf{};
            localtime_r(&t, &tm_buf);
            std::strftime(tbuf, sizeof(tbuf), "%H:%M", &tm_buf);

            std::string msg = std::string("[") + tbuf + "] [Client "
                            + std::to_string(cid) + "]: " + payload;

            broadcast(cid, MsgType::CHAT, msg);
        }
    } catch (const std::exception& e) {
        remove_client(session->id());
        log("[ERROR] Client " + std::to_string(session->id())
            + " thread exception: " + e.what());
    } catch (...) {
        remove_client(session->id());
        log("[ERROR] Client " + std::to_string(session->id())
            + " thread unknown exception");
    }
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

        // Assign a sequential id atomically so that simultaneous accepts on
        // multiple threads (Phase 4) would still produce unique ids.
        int cid = next_id_.fetch_add(1);
        auto session = std::make_shared<ClientSession>(
            cid, Socket(cli_fd), peer_str);

        add_client(session);

        {
            std::lock_guard<std::mutex> lk(registry_mutex_);
            log("[INFO] Client " + std::to_string(cid)
                + " connected from " + peer_str
                + " (" + std::to_string(clients_.size()) + " online)");
        }

        // Spawn a detached thread for this client's recv loop.
        // Phase 4 will replace detached threads with tracked, joined threads
        // for graceful shutdown.
        std::thread t(&Server::client_thread, this, session);
        t.detach();
    }
}
