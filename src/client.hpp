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
//   - Receiver wins  -> the server went away: the reason is stored, exit code 1.
//   - Main thread wins -> the user quit (/quit, Ctrl+D, Ctrl+C): it sends QUIT,
//     and the receiver is only woken up by our own shutdown() and must stay
//     silent, exit code 0.
// The stored reason is printed by run() AFTER the terminal has been put back
// into its normal mode, so it never lands in a raw-mode screen.

#include "socket_utils.hpp"
#include "protocol.hpp"
#include "terminal.hpp"

#include <atomic>
#include <memory>
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
    std::string         end_message_;     // why the session failed; written by whoever wins running_
    std::unique_ptr<Screen> screen_;
    KeyDecoder          decoder_;         // main thread only

    // Sends JOIN and waits for the verdict.  Returns false (after telling the
    // user why) if the chat cannot be entered.
    bool join();

    void receiver_loop();

    // Reads stdin until the user quits, input ends or the session fails.
    void input_loop();
    bool service_terminal();                                        // signals + resize; false = quit
    bool handle_plain(const char* data, std::size_t n, std::string& line_buf);
    bool handle_keys(const std::vector<Key>& keys);

    // Says goodbye (if the user ended the session), closes the connection and
    // waits for the receiver thread.
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

    // Ends the session with exit code 1 and remembers 'message' for run() to
    // show.  Only the first caller counts (see the running_ rule above).
    void fail(const std::string& message);
};
