#pragma once
// server.hpp – Server and ClientSession classes.
//
// Threading model:
//   - Accept thread (the caller of Server::run): poll()s the listening socket
//     with a 200 ms timeout so it notices SIGINT/SIGTERM promptly, accepts
//     connections, starts one tracked std::thread per connection and reaps the
//     threads that have finished.  It also performs the shutdown.
//   - One std::thread per connection: the recv_frame loop.  When the
//     connection ends it cleans up after itself and raises
//     Connection::finished as its very last act; only then does the accept
//     thread join it.  There are no detached threads and nothing is joined
//     twice (a Connection leaves the list before it is joined).
//
// Connection state: every connection starts as AwaitJoin (not in the registry:
// it receives no broadcasts and does not count as online).  A valid, unused
// JOIN moves it to Joined (in the registry) for the rest of its life.
//
// Graceful shutdown: SIGINT/SIGTERM only set an atomic flag.  The accept loop
// sees it and then (1) closes the listening socket, (2) sends SYSTEM(shutdown)
// to every joined user without ever waiting on a stalled one, (3) calls
// shutdown(SHUT_RDWR) on EVERY connection, joined or not, so threads blocked
// in recv() or in a send() wake up, (4) joins all client threads, (5) logs a
// summary.  A second signal exits at once.
//
// Shared state, who protects it, and the lock order
//   registry_mutex_        clients_ (the name -> session map).  A leaf lock held
//                          only for map operations: NEVER across socket I/O or
//                          logging, and never together with another lock.
//   ClientSession::send_mutex_
//                          one per session; serialises frames on its socket and
//                          is held DURING send().  The send timeout bounds that.
//   g_log_mutex            (server.cpp) serialises log lines; innermost lock.
//   connections_           no mutex: only the accept thread touches the list
//                          (add, reap, shutdown), so it cannot race.
//   stopping_, peak_users_, relayed_
//                          atomics, readable from every thread.
// Lock order, outermost first: send_mutex_ -> registry_mutex_ -> g_log_mutex.
// Nothing ever takes them in another order.  handle_join() is the only place
// that holds two at once (the new session's send_mutex_, then briefly the
// registry, and it may log while still holding send_mutex_); everything else
// holds at most one, and in particular never two send_mutex_ at the same time.

#include "socket_utils.hpp"
#include "protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// ── ClientSession ─────────────────────────────────────────────────────────────
// Represents one connected client.  Owned by a shared_ptr so that the registry
// and the per-client thread can both hold references safely.  The socket is
// closed only when the last reference disappears, so a broadcaster that still
// holds a snapshot can never write to a recycled file descriptor.
class ClientSession {
public:
    using OutputLock = std::unique_lock<std::timed_mutex>;

    ClientSession(Socket sock, std::string peer_addr);

    // Thread-safe send: the send_mutex prevents two threads from interleaving
    // bytes of different frames on this socket.
    FrameStatus send_frame(MsgType type, const std::string& payload);

    // For a sequence of frames that must stay together (JOIN_OK followed by the
    // welcome line): take the output lock once, then send with the overload
    // below, which expects that lock to be held.
    OutputLock  lock_output();
    FrameStatus send_frame(const OutputLock& held, MsgType type, const std::string& payload);

    // Best effort that never blocks for long: gives up (IO_ERROR) if the output
    // lock is not free by 'deadline' (another thread is stuck sending to this
    // client) or if the kernel send buffer is full (MSG_DONTWAIT).  Used for
    // the shutdown notice, where a stalled reader must not delay everybody.
    FrameStatus try_send_frame(MsgType type, const std::string& payload,
                               std::chrono::steady_clock::time_point deadline);

    int                fd()        const noexcept { return sock_.fd(); }
    const std::string& peer_addr() const noexcept { return peer_addr_; }

    // Empty until the client has joined.  set_name() is called once, under the
    // registry lock and before other threads can see the session.
    const std::string& name()   const noexcept { return name_; }
    bool               joined() const noexcept { return !name_.empty(); }
    void               set_name(std::string name) { name_ = std::move(name); }

    // Set by the session's own thread once its connection has ended (quit,
    // error or shutdown).  A broadcaster whose send fails afterwards has simply
    // lost a race with a departing user and reports nothing.
    void mark_closing() noexcept { closing_.store(true); }
    bool closing() const noexcept { return closing_.load(); }

private:
    Socket            sock_;
    std::string       peer_addr_;
    std::string       name_;
    std::timed_mutex  send_mutex_;       // serialises writes to sock_
    std::atomic<bool> closing_{false};
};

using SessionPtr = std::shared_ptr<ClientSession>;

// ── Server ────────────────────────────────────────────────────────────────────
class Server {
public:
    explicit Server(uint16_t port);
    ~Server();    // safety net: no client thread may outlive the object

    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    // Runs the accept loop until SIGINT or SIGTERM, then shuts down gracefully
    // (see above) and returns.  Throws NetError if the server cannot start.
    void run();

private:
    // Why a client's connection ended (for the log line and the leave notice).
    struct Disconnect {
        bool        clean;   // true only for an explicit QUIT
        std::string reason;
    };

    // Resource limits; the defaults can be overridden by test-only environment
    // variables (documented in server.cpp).
    struct Limits {
        std::size_t max_clients;           // connections, joined or not
        int         handshake_timeout_s;   // silence allowed before JOIN completes
        int         send_timeout_s;        // longest a send() may block
    };

    // One accepted connection and the thread serving it.
    struct Connection {
        explicit Connection(SessionPtr s) : session(std::move(s)) {}
        SessionPtr        session;
        std::thread       thread;
        std::atomic<bool> finished{false};   // raised by the thread as its last act
    };

    uint16_t port_;
    Limits   limits_;
    std::chrono::steady_clock::time_point started_;

    std::atomic<bool>          stopping_{false};    // shutdown has begun: stay quiet
    std::atomic<std::size_t>   peak_users_{0};      // most users online at once
    std::atomic<std::uint64_t> relayed_{0};         // chat + private messages relayed

    // Registry of joined users, keyed by the LOWERCASE username so that "Bob"
    // and "bob" collide.  Iterating it therefore yields case-insensitive order.
    // Guarded by registry_mutex_.
    std::map<std::string, SessionPtr> clients_;
    std::mutex registry_mutex_;

    // Every accepted connection (joined or not), for shutdown(), joining and
    // the connection limit.  Accept thread only (see the lock table above).
    std::list<Connection> connections_;

    // ── accept side (accept thread) ──
    // Accepts one pending connection and starts, or rejects, it.  'spare' is a
    // descriptor kept in reserve for the EMFILE case.
    void accept_connection(int listen_fd, Socket& spare);
    void accept_failed(int err, int listen_fd, Socket& spare);
    std::size_t live_connections() const;
    void reap_finished();         // joins and forgets the threads that have finished
    void announce_shutdown();     // SYSTEM(shutdown) to joined users, time-boxed
    void close_connections();     // shutdown() every socket, then join every thread

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
    // (its own thread then notices and cleans up).  Returns false on failure.
    bool deliver(const SessionPtr& session, MsgType type, const std::string& payload);

    // Tells ONE user something went wrong (a SYSTEM frame of kind error).
    void send_error(const SessionPtr& session, const std::string& text);

    // The joined session registered under 'name' (any letter case), or null.
    SessionPtr find_user(const std::string& name);
    void report_send_failure(const ClientSession& session, FrameStatus status, int err);

    // ── per-client side (client threads) ──
    // Entry point of a client thread.  Catches every exception (an exception
    // escaping a thread would call std::terminate and kill the whole server),
    // performs the one-and-only cleanup and finally raises '*finished'.
    void client_thread(SessionPtr session, std::atomic<bool>* finished);

    // The receive loop.  Returns when the connection ends, saying why.
    Disconnect serve_client(const SessionPtr& session);

    // One decoded frame.  Returns nullopt to keep the connection open, or the
    // reason to drop it.
    std::optional<Disconnect> handle_frame(const SessionPtr& session, MsgType type,
                                           const std::string& payload);
    std::optional<Disconnect> handle_join(const SessionPtr& session, const std::string& name);
    std::optional<Disconnect> handle_say(const SessionPtr& session, const std::string& payload);
    std::optional<Disconnect> handle_priv_say(const SessionPtr& session, const std::string& payload);
    void                      handle_list(const SessionPtr& session);

    // Answers a failed JOIN with JOIN_ERR; the caller then drops the connection.
    Disconnect reject_join(const SessionPtr& session, const std::string& reason);
};
