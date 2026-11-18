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
        throw NetError("getaddrinfo(" + host + "): "
                       + std::string(gai_strerror(gai_err)));
    }

    // Try each result in order until one connects successfully.
    int conn_fd = -1;
    int saved_errno = 0;
    for (addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
        int fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        // connect() initiates the TCP three-way handshake.  It blocks until
        // the kernel receives the SYN-ACK from the server and sends the final
        // ACK, or until it times out or is refused.
        if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            conn_fd = fd;
            break;
        }
        saved_errno = errno;
        ::close(fd);
    }
    ::freeaddrinfo(res);  // must always free, even on error

    if (conn_fd < 0) {
        errno = saved_errno;
        throw NetError("connect() to " + host + ":" + port_str);
    }

    sock_ = Socket(conn_fd);
    std::cout << "Connected to " << host << ":" << port << '\n';
    std::cout.flush();
}

// ── Receiver thread ───────────────────────────────────────────────────────────
void Client::receiver_loop() {
    try {
        while (running_.load()) {
            MsgType     type;
            std::string payload;
            FrameStatus st = recv_frame(sock_.fd(), type, payload);

            if (st == FrameStatus::OK) {
                // Payload is binary-safe: printed with write() semantics via cout.
                std::cout << payload << '\n';
                std::cout.flush();
                continue;
            }

            // If the user ended input cleanly (Ctrl+D), do not treat peer closure as an error.
            if (user_eof_.load()) {
                return;
            }

            // Any non-OK status means the server connection is gone unexpectedly.
            running_.store(false);
            exit_code_ = 1;

            if (st == FrameStatus::PEER_CLOSED) {
                std::cout << "*** Server closed the connection\n";
            } else if (st == FrameStatus::PEER_CLOSED_MIDFRAME) {
                std::cout << "*** Server closed the connection (mid-frame)\n";
            } else if (st == FrameStatus::IO_ERROR) {
                std::cout << "*** Connection error: " << std::strerror(errno) << '\n';
            } else {
                std::cout << "*** Protocol error from server\n";
            }
            std::cout.flush();
        }
    } catch (...) {
        if (!user_eof_.load()) {
            running_.store(false);
            exit_code_ = 1;
        }
    }
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

    while (!stdin_eof) {
        if (!running_.load()) break;

        pollfd pfd{};
        pfd.fd     = STDIN_FILENO;
        pfd.events = POLLIN;

        int ret = ::poll(&pfd, 1, 200);  // 200 ms timeout
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) continue; // timeout – loop back to check running_

        if (pfd.revents & POLLERR) break;
        // On a pipe or FIFO, when the peer closes the write end and no data
        // remains, poll() reports POLLHUP (without POLLIN).  We must check
        // both so that EOF triggers a read() that returns 0.
        if (!(pfd.revents & (POLLIN | POLLHUP))) continue;

        // read() may give us multiple lines or a partial line in one call.
        ssize_t n = ::read(STDIN_FILENO, read_buf, sizeof(read_buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) {
            // EOF (Ctrl+D): send whatever is in the buffer if non-empty.
            stdin_eof = true;
        }

        for (ssize_t i = 0; i < n; ++i) {
            char c = read_buf[i];
            if (c == '\n') {
                // Strip a trailing '\r' so the client works correctly when
                // stdin comes from a Windows-style CRLF source (e.g., a pipe).
                if (!line_buf.empty() && line_buf.back() == '\r')
                    line_buf.pop_back();

                if (!line_buf.empty()) {
                    if (line_buf.size() > MAX_CHAT_TEXT) {
                        std::cout << "Message too long (max 1024 bytes), not sent\n";
                        std::cout.flush();
                    } else if (running_.load()) {
                        send_frame(sock_.fd(), MsgType::CHAT, line_buf);
                    }
                }
                line_buf.clear();
            } else {
                line_buf += c;
            }
        }
    }

    // Send the last unterminated line at EOF if it is non-empty.
    if (!line_buf.empty() && running_.load()) {
        if (line_buf.back() == '\r') line_buf.pop_back();
        if (!line_buf.empty()) {
            if (line_buf.size() > MAX_CHAT_TEXT) {
                std::cout << "Message too long (max 1024 bytes), not sent\n";
                std::cout.flush();
            } else {
                send_frame(sock_.fd(), MsgType::CHAT, line_buf);
            }
        }
    }

    // If the loop ended due to stdin EOF (Ctrl+D), record that this was a clean
    // user-initiated exit so the receiver thread does not treat connection shutdown
    // as a failure.
    if (stdin_eof) {
        user_eof_.store(true);
        running_.store(false);
        exit_code_ = 0;
    }

    // shutdown() sends a TCP FIN to the server and unblocks the receiver
    // thread's recv_frame call, which will then see PEER_CLOSED and exit.
    ::shutdown(sock_.fd(), SHUT_RDWR);

    if (recv_thread_.joinable()) recv_thread_.join();

    return exit_code_;
}
