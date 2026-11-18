// client.cpp – Client implementation.

#include "client.hpp"

#include <sys/socket.h>   // socket, connect, shutdown
#include <netdb.h>        // getaddrinfo, freeaddrinfo
#include <unistd.h>       // read, close
#include <poll.h>         // poll

#include <cstring>        // strerror, memset
#include <cstdio>         // fprintf, printf
#include <cerrno>
#include <iostream>
#include <string>

// ── Constructor: resolve host and connect ─────────────────────────────────────
Client::Client(const std::string& host, uint16_t port)
    : sock_(-1), host_(host), port_(port)
{
    // getaddrinfo() performs DNS resolution and returns a linked list of
    // candidate addresses.  Using it (instead of gethostbyname) is thread-safe
    // and supports both IPv4 and IPv6 without code changes.
    addrinfo hints{};
    hints.ai_family   = AF_INET;      // Phase 1: IPv4 only
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

// ── Receiver thread ───────────────────────────────────────────────────────────
void Client::receiver_loop() {
    while (running_.load()) {
        MsgType     type;
        std::string payload;
        FrameStatus st = recv_frame(sock_.fd(), type, payload);
        const int   err = errno;   // only meaningful for IO_ERROR

        if (st == FrameStatus::OK) {
            // Payload is binary-safe: printed with write() semantics via cout.
            std::cout << payload << '\n';
            std::cout.flush();
            continue;
        }

        // Any non-OK status ends the session.  exchange() tells us who got
        // there first: if running_ was already false, the main thread is
        // quitting on purpose (Ctrl+D) and its shutdown() is what woke us up,
        // so there is nothing to report and the exit code stays 0.
        if (!running_.exchange(false)) return;
        exit_code_.store(1);

        if (st == FrameStatus::PEER_CLOSED) {
            std::cout << "*** Server closed the connection\n";
        } else if (st == FrameStatus::PEER_CLOSED_MIDFRAME) {
            std::cout << "*** Server closed the connection (mid-frame)\n";
        } else if (st == FrameStatus::IO_ERROR) {
            std::cout << "*** Connection error: " << std::strerror(err) << '\n';
        } else {
            std::cout << "*** Protocol error from server\n";
        }
        std::cout.flush();
    }
}

// ── Helpers used by the main thread ───────────────────────────────────────────
void Client::fail(const std::string& message) {
    // Same "who got there first" rule as the receiver: report only once.
    if (running_.exchange(false)) {
        exit_code_.store(1);
        std::cout << "*** " << message << '\n';
        std::cout.flush();
    }
}

bool Client::send_line(std::string line) {
    // Strip a trailing '\r' so the client works correctly when stdin comes
    // from a Windows-style CRLF source (e.g., a pipe).
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) return true;           // never send empty messages

    if (line.size() > MAX_CHAT_TEXT) {
        std::cout << "Message too long (max 1024 bytes), not sent\n";
        std::cout.flush();
        return true;                         // connection is still fine
    }

    if (send_frame(sock_.fd(), MsgType::CHAT, line) != FrameStatus::OK) {
        fail(std::string("Send failed: ") + std::strerror(errno));
        return false;
    }
    return true;
}

// ── Main loop: stdin → send ───────────────────────────────────────────────────
int Client::run() {
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
    // the user quit; if false the receiver (or fail()) already reported a
    // problem and set the exit code.
    running_.exchange(false);

    // shutdown() sends a TCP FIN to the server and unblocks the receiver
    // thread's recv() call, which then sees running_ == false and exits quietly.
    ::shutdown(sock_.fd(), SHUT_RDWR);

    if (recv_thread_.joinable()) recv_thread_.join();

    return exit_code_.load();
}
