#pragma once
// client.hpp – Client class.
//
// Threading model:
//   Receiver thread: loops on recv_frame, prints to stdout.
//   Main thread:     reads stdin via poll(), sends CHAT frames.
//
// The 'running' atomic flag links the two threads and also records WHO ended
// the session.  Whichever thread flips it from true to false first "wins":
//   - Receiver wins  -> the server went away: print a message, exit code 1.
//   - Main thread wins -> the user quit (Ctrl+D): the receiver is only woken
//     up by our own shutdown() and must stay silent, exit code 0.

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
    std::atomic<int>    exit_code_{0};
    std::thread         recv_thread_;

    void receiver_loop();

    // Validates and sends one typed line.  Returns false if the connection
    // is no longer usable (the caller should stop reading stdin).
    bool send_line(std::string line);

    // Called when the main thread itself detects a fatal problem.
    void fail(const std::string& message);
};
