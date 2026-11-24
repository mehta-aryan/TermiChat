#pragma once
// client.hpp – Client class.
//
// Startup: connect, send JOIN, then wait SYNCHRONOUSLY for JOIN_OK or
// JOIN_ERR.  Only after JOIN_OK do the two threads below start (and, in a
// real terminal, the UI: raw mode, signal handlers, banner).
//
// Two modes, chosen once from the terminal (see detect_terminal):
//   interactive – stdin and stdout are terminals: styled output, a one-row
//                 line editor, history, /clear;
//   plain       – anything else (pipes, tests): the old line-by-line text
//                 formats, no colour, no prompt, stdin read line by line.
//
// Threading model:
//   Receiver thread: loops on recv_frame, turns each frame into a ChatEvent
//                    and hands it to the Screen (the only place that prints chat).
//   Main thread:     reads stdin via poll(), sends SAY / PRIV_SAY frames and runs /commands.
//
// The 'running' atomic flag links the two threads and also records WHO ended
// the session.  Whichever thread flips it from true to false first "wins":
//   - Receiver wins  -> the connection ended without the user asking for it.
//     If the server had announced its shutdown (SYSTEM kind=shutdown) first:
//     "Server shut down", exit code 0.  Otherwise: "Connection lost", exit 1.
//   - Main thread wins -> the user quit (/quit, Ctrl+D, Ctrl+C, SIGTERM; the
//     signal handlers are installed in plain mode too): silent, exit code 0.
// The stored reason is printed by run() AFTER the terminal has been put back
// into its normal mode, so it never lands in a raw-mode screen.
//
// A user quit is graceful: send QUIT, half-close with shutdown(SHUT_WR) (our
// FIN), keep reading until the server closes its side or 2 seconds pass, then
// close.  The receiver thread does that reading; see Client::disconnect() for
// why a plain close() is avoided.

#include "socket_utils.hpp"
#include "protocol.hpp"
#include "terminal.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Screen;       // the terminal UI; defined in client.cpp

class Client {
public:
    // Resolves 'host', connects to 'port', throws NetError on failure.
    // 'username' must already have passed check_username().
    Client(const std::string& host, uint16_t port, std::string username);
    ~Client();      // out of line: Screen is an incomplete type here

    // Joins the chat, then runs the main stdin-read loop.
    // Returns 0 (user quit, or the server announced its shutdown), 1 (server
    // gone / network error / join refused).
    int run();

private:
    Socket              sock_;
    std::string         host_;
    uint16_t            port_;
    std::string         username_;
    std::atomic<bool>   running_{true};
    std::atomic<int>    exit_code_{0};
    std::atomic<bool>   server_notified_{false};   // a SYSTEM(shutdown) frame has arrived
    std::thread         recv_thread_;
    std::string         end_message_;     // how the session ended; written by whoever wins running_
    SystemKind          end_kind_ = SystemKind::Error;   // ...and its kind (error / shutdown)
    std::unique_ptr<Screen> screen_;
    KeyDecoder          decoder_;         // main thread only

    // Lets disconnect() wait (with a timeout) until the receiver has read the
    // connection to its end.
    std::mutex              done_mu_;
    std::condition_variable done_cv_;
    bool                    receiver_done_ = false;   // guarded by done_mu_

    // Sends JOIN and waits for the verdict.  Returns false (after telling the
    // user why) if the chat cannot be entered.
    bool join();

    // Thread entry: runs receive_events() and then flags receiver_done_.
    void receiver_loop();
    void receive_events();

    // Reads stdin until the user quits, input ends or the session fails.
    void input_loop();
    bool service_terminal();                                        // signals + resize; false = quit
    bool handle_plain(const char* data, std::size_t n, std::string& line_buf);
    bool handle_keys(const std::vector<Key>& keys);

    // If the user ended the session: says goodbye (QUIT), half-closes and
    // drains the connection for up to 2 s.  Then makes sure the receiver is
    // awake and joins it.
    void disconnect();

    // Handles one typed line: a /command, or chat text to send.  Returns false
    // when the stdin loop should stop: the connection is unusable, or the user
    // asked to quit.
    bool send_line(std::string line);
    bool run_command(const std::string& line);

    // /msg: 'args' is everything after the command word.  Shows a local notice
    // (and sends nothing) for bad input; otherwise sends one PRIV_SAY.
    bool send_private(const std::string& args);

    // A message from the client itself (help, errors) shown like a server notice.
    void notice(SystemKind kind, const std::string& text);

    // Ends the session without the user asking for it and remembers how for
    // run() to show: exit code 1 with 'message', or, if the server announced its
    // shutdown first, exit code 0 with "Server shut down".  Only the first
    // caller counts (see the running_ rule above).
    void fail(const std::string& message);
};
