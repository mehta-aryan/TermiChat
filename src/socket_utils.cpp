// socket_utils.cpp – implementation of send_all and recv_all.

#include "socket_utils.hpp"

#include <sys/socket.h>
#include <cerrno>

IOStatus send_all(int fd, const void* buf, std::size_t len, int flags) {
    const auto* ptr = static_cast<const char*>(buf);
    std::size_t sent = 0;

    while (sent < len) {
        // MSG_NOSIGNAL: suppress SIGPIPE; we get EPIPE as an errno instead.
        ssize_t n = ::send(fd, ptr + sent, len - sent, MSG_NOSIGNAL | flags);
        if (n < 0) {
            if (errno == EINTR) continue;   // interrupted by signal – retry
            return IOStatus::ERROR;
        }
        sent += static_cast<std::size_t>(n);
    }
    return IOStatus::OK;
}

IOStatus recv_all(int fd, void* buf, std::size_t len, std::size_t* bytes_read) {
    auto* ptr = static_cast<char*>(buf);
    std::size_t got = 0;
    IOStatus status = IOStatus::OK;

    while (got < len) {
        // recv() returns 0 when the peer has sent a TCP FIN (clean close).
        ssize_t n = ::recv(fd, ptr + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;   // interrupted by signal – retry
            status = IOStatus::ERROR;       // errno is left untouched below
            break;
        }
        if (n == 0) { status = IOStatus::PEER_CLOSED; break; }
        got += static_cast<std::size_t>(n);
    }
    if (bytes_read) *bytes_read = got;
    return status;
}
