#pragma once
// client.hpp – Client class.
//
// Threading model:
//   Receiver thread: loops on recv_frame, prints to stdout.
//   Main thread:     reads stdin via poll(), sends CHAT frames.
//
// The 'running' atomic flag links the two threads:
//   - Receiver sets it false when the server closes or errors.
//   - Main thread checks it each poll() iteration and exits within ~200 ms.

#include "socket_utils.hpp"
#include "protocol.hpp"

#include <string>
#include <atomic>
#include <thread>

class Client {
public:
    // Resolves 'host', connects to 'port', throws NetError on failure.
    Client(const std::string& host, uint16_t port);

    // Runs the main stdin-read loop.  Returns 0 (user EOF) or 1 (server error).
    int run();

private:
    Socket              sock_;
    std::string         host_;
    uint16_t            port_;
    std::atomic<bool>   running_{true};
    std::atomic<bool>   user_eof_{false};
    int                 exit_code_{0};
    std::thread         recv_thread_;

    void receiver_loop();
};
