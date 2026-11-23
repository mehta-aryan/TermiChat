#pragma once
// client.hpp – Client class.
//
// Startup: connect, send JOIN, then wait SYNCHRONOUSLY for JOIN_OK or
// JOIN_ERR.  Only after JOIN_OK do the two threads below start.
//
// Threading model:
//   Receiver thread: loops on recv_frame, turns each frame into a ChatEvent
//                    and hands it to show() (the only place that prints chat).
//   Main thread:     reads stdin via poll(), sends SAY / PRIV_SAY frames and runs /commands.
//
// The 'running' atomic flag links the two threads and also records WHO ended
// the session.  Whichever thread flips it from true to false first "wins":
//   - Receiver wins  -> the server went away: print a message, exit code 1.
//   - Main thread wins -> the user quit (/quit or Ctrl+D): it sends QUIT, and
//     the receiver is only woken up by our own shutdown() and must stay
//     silent, exit code 0.

#include "socket_utils.hpp"
#include "protocol.hpp"

#include <string>
#include <atomic>
#include <thread>

class Client {
public:
    // Resolves 'host', connects to 'port', throws NetError on failure.
    // 'username' must already have passed check_username().
    Client(const std::string& host, uint16_t port, std::string username);

    // Joins the chat, then runs the main stdin-read loop.
    // Returns 0 (user quit), 1 (server error / join refused).
    int run();

private:
    Socket              sock_;
    std::string         host_;
    uint16_t            port_;
    std::string         username_;
    std::atomic<bool>   running_{true};
    std::atomic<int>    exit_code_{0};
    std::thread         recv_thread_;

    // Sends JOIN and waits for the verdict.  Returns false (after telling the
    // user why) if the chat cannot be entered.
    bool join();

    void receiver_loop();

    // Handles one typed line: a /command, or chat text to send.  Returns false
    // when the stdin loop should stop: the connection is unusable, or the user
    // asked to quit.
    bool send_line(std::string line);
    bool run_command(const std::string& line);

    // /msg: 'args' is everything after the command word.  Shows a local notice
    // (and sends nothing) for bad input; otherwise sends one PRIV_SAY.
    bool send_private(const std::string& args);

    // Called when the main thread itself detects a fatal problem.
    void fail(const std::string& message);
};
