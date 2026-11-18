#pragma once
// socket_utils.hpp – RAII Socket wrapper, NetError, and reliable I/O helpers.
// Every syscall in this file is explained with a "why" comment.

#include <stdexcept>
#include <string>
#include <cerrno>
#include <cstring>

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

// ── NetError ────────────────────────────────────────────────────────────────
// Thrown during setup (socket/bind/listen/connect) where a failure is fatal.
// I/O inside per-client loops uses return statuses instead of exceptions so
// that one bad client cannot unwind another's stack.
class NetError : public std::runtime_error {
public:
    // Format: "<what>: <strerror(errno)>"
    explicit NetError(const std::string& what_arg)
        : std::runtime_error(what_arg + ": " + std::strerror(errno)) {}
};

// ── Socket ───────────────────────────────────────────────────────────────────
// Move-only RAII wrapper.  The destructor calls close() so callers never
// scatter manual close() calls, which would be error-prone in exception paths.
class Socket {
public:
    explicit Socket(int fd) noexcept : fd_(fd) {}

    // Allow move: the moved-from object becomes a "null" socket (fd_ = -1).
    Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close_fd();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    // No copy: two owners of the same fd would double-close.
    Socket(const Socket&)            = delete;
    Socket& operator=(const Socket&) = delete;

    ~Socket() { close_fd(); }

    int fd() const noexcept { return fd_; }

    // Release ownership without closing – used when transferring to another owner.
    int release() noexcept { int f = fd_; fd_ = -1; return f; }

private:
    int fd_;

    void close_fd() noexcept {
        // close() releases the kernel's file-description reference.
        // We only close when fd_ is valid (>= 0).
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }
};

// ── I/O return status ────────────────────────────────────────────────────────
enum class IOStatus { OK, PEER_CLOSED, ERROR };

// ── send_all ─────────────────────────────────────────────────────────────────
// TCP send() may write fewer bytes than requested (partial send) or be
// interrupted by a signal (EINTR).  This loop guarantees all 'len' bytes are
// delivered to the kernel send buffer before returning.
//
// MSG_NOSIGNAL: if the peer has closed the connection, send() raises SIGPIPE
// by default, which terminates the process.  MSG_NOSIGNAL turns that into a
// return of -1 / EPIPE so we can handle it gracefully.
IOStatus send_all(int fd, const void* buf, std::size_t len);

// ── recv_all ─────────────────────────────────────────────────────────────────
// TCP recv() may return fewer bytes than requested (partial read – the kernel
// only gives us what has arrived so far).  This loop reads until exactly 'len'
// bytes have been accumulated.
//
// Returns:
//   OK          – all bytes read
//   PEER_CLOSED – recv() returned 0 before we got all bytes (clean TCP FIN)
//   ERROR       – a real socket error; errno is preserved
IOStatus recv_all(int fd, void* buf, std::size_t len);
