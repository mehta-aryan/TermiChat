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
// Connection state: every connection starts as AwaitJoin (not in the registry:
// it receives no broadcasts and does not count as online).  A valid, unused
// JOIN moves it to Joined (in the registry) for the rest of its life.
//
// Concurrency invariant: the global registry mutex is held only for map
// operations (insert/erase/snapshot).  It is NEVER held during socket I/O or
// logging, so one slow or dead client cannot block others.

#include "socket_utils.hpp"
#include "protocol.hpp"

#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

// ── ClientSession ─────────────────────────────────────────────────────────────
// Represents one connected client.  Owned by a shared_ptr so that the registry
// and the per-client thread can both hold references safely.  The socket is
// closed only when the last reference disappears, so a broadcaster that still
// holds a snapshot can never write to a recycled file descriptor.
class ClientSession {
public:
    using OutputLock = std::unique_lock<std::mutex>;

    ClientSession(Socket sock, std::string peer_addr);

    // Thread-safe send: the send_mutex prevents two threads from interleaving
    // bytes of different frames on this socket.
    FrameStatus send_frame(MsgType type, const std::string& payload);

    // For a sequence of frames that must stay together (JOIN_OK followed by the
    // welcome line): take the output lock once, then send with the overload
    // below, which expects that lock to be held.
    OutputLock  lock_output();
    FrameStatus send_frame(const OutputLock& held, MsgType type, const std::string& payload);

    int                fd()        const noexcept { return sock_.fd(); }
    const std::string& peer_addr() const noexcept { return peer_addr_; }

    // Empty until the client has joined.  set_name() is called once, under the
    // registry lock and before other threads can see the session.
    const std::string& name()   const noexcept { return name_; }
    bool               joined() const noexcept { return !name_.empty(); }
    void               set_name(std::string name) { name_ = std::move(name); }

private:
    Socket      sock_;
    std::string peer_addr_;
    std::string name_;
    std::mutex  send_mutex_; // serialises writes to sock_
};

using SessionPtr = std::shared_ptr<ClientSession>;

// ── Server ────────────────────────────────────────────────────────────────────
class Server {
public:
    explicit Server(uint16_t port);

    // Runs the accept() loop until the process exits.  Never returns normally.
    void run();

private:
    // Why a client's connection ended (for the log line and the leave notice).
    struct Disconnect {
        bool        clean;   // true only for an explicit QUIT
        std::string reason;
    };

    uint16_t port_;

    // Registry of joined users, keyed by the LOWERCASE username so that "Bob"
    // and "bob" collide.  Iterating it therefore yields case-insensitive order.
    // Guarded by registry_mutex_.
    std::map<std::string, SessionPtr> clients_;
    std::mutex registry_mutex_;

    // Atomically claims 'name' for 'session'.  Returns nullopt if the name is
    // taken; otherwise the sessions online right after the insert (including
    // 'session' itself), in name order.
    std::optional<std::vector<SessionPtr>> register_client(const SessionPtr& session,
                                                           const std::string& name);

    // Removes a joined session; returns the sessions that remain.
    std::vector<SessionPtr> unregister_client(const SessionPtr& session);

    // Everyone who is currently joined, in name order.
    std::vector<SessionPtr> snapshot();

    // Sends one frame to every session in 'targets'.  The caller obtained the
    // list under the registry lock and has released it, so no lock is held
    // while sending.
    void broadcast(const std::vector<SessionPtr>& targets, MsgType type,
                   const std::string& payload);

    // Sends one frame to one session; on failure logs and shuts its socket down
    // (its own thread then notices and cleans up).
    void deliver(const SessionPtr& session, MsgType type, const std::string& payload);
    void report_send_failure(const ClientSession& session, FrameStatus status, int err);

    // Entry point for the per-client detached thread.  Catches every
    // exception (an exception escaping a thread would call std::terminate and
    // kill the whole server) and performs the one-and-only cleanup.
    void client_thread(SessionPtr session);

    // The receive loop.  Returns when the connection ends, saying why.
    Disconnect serve_client(const SessionPtr& session);

    // One decoded frame.  Returns nullopt to keep the connection open, or the
    // reason to drop it.
    std::optional<Disconnect> handle_frame(const SessionPtr& session, MsgType type,
                                           const std::string& payload);
    std::optional<Disconnect> handle_join(const SessionPtr& session, const std::string& name);
    std::optional<Disconnect> handle_say(const SessionPtr& session, const std::string& payload);
    void                      handle_list(const SessionPtr& session);

    // Answers a failed JOIN with JOIN_ERR; the caller then drops the connection.
    Disconnect reject_join(const SessionPtr& session, const std::string& reason);
};
