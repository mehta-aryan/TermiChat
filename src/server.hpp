#pragma once
// server.hpp – Server and ClientSession classes.
//
// Threading model:
//   - Main thread: accept() loop only.  No I/O on accepted sockets.
//   - Per-client detached thread: recv_frame loop.  Cleans up itself when
//     the connection closes or errors.
//   Phase 4 will replace detached threads with tracked, joined threads
//   to enable graceful shutdown.
//
// Concurrency invariant: the global registry mutex is held only for map
// operations (add/remove/snapshot).  It is NEVER held during socket I/O so
// that one slow or dead client cannot block others.

#include "socket_utils.hpp"
#include "protocol.hpp"

#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>

// ── ClientSession ─────────────────────────────────────────────────────────────
// Represents one connected client.  Owned by a shared_ptr so that the registry
// and the per-client thread can both hold references safely.
class ClientSession {
public:
    ClientSession(int id, Socket sock, std::string peer_addr);

    // Thread-safe send: the send_mutex prevents two threads from interleaving
    // bytes of different frames on this socket.
    FrameStatus send_frame(MsgType type, const std::string& payload);

    int         id()        const noexcept { return id_; }
    int         fd()        const noexcept { return sock_.fd(); }
    const std::string& peer_addr() const noexcept { return peer_addr_; }

private:
    int         id_;
    Socket      sock_;
    std::string peer_addr_;
    std::mutex  send_mutex_; // serialises writes to sock_
};

// ── Server ────────────────────────────────────────────────────────────────────
class Server {
public:
    explicit Server(uint16_t port);

    // Runs the accept() loop until the process exits.  Never returns normally.
    void run();

private:
    uint16_t port_;
    std::atomic<int> next_id_{1};  // monotonically increasing client id

    // Registry: maps client id → session.  Guarded by registry_mutex_.
    std::map<int, std::shared_ptr<ClientSession>> clients_;
    std::mutex registry_mutex_;

    void add_client(std::shared_ptr<ClientSession> session);
    void remove_client(int id);

    // Sends a frame to every client except 'sender_id'.
    // Copies the session pointers under the lock, releases it, then sends.
    void broadcast(int sender_id, MsgType type, const std::string& payload);

    // Entry point for the per-client detached thread.
    void client_thread(std::shared_ptr<ClientSession> session);
};
