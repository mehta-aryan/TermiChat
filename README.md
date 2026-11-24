# TermiChat

A multi-client chat application for the terminal, written in **C++17** using raw
**POSIX sockets** and threads. One server relays messages between any number of
clients over TCP. The implementation and this README document the networking,
concurrency, and terminal I/O decisions behind it.

Made by [mehta-aryan](https://github.com/mehta-aryan).

Licensed under the MIT License (see `LICENSE`).

## Contents

1. [Overview](#overview)
2. [Requirements and install](#requirements-and-install)
3. [Build, run, flags and commands](#build-run-flags-and-commands)
4. [Architecture](#architecture)
5. [Networking explained](#networking-explained)
6. [Protocol](#protocol)
7. [Concurrency design](#concurrency-design)
8. [Error handling and exit codes](#error-handling-and-exit-codes)
9. [Testing](#testing)
10. [Packet analysis with Wireshark](#packet-analysis-with-wireshark)
11. [Limitations and future work](#limitations-and-future-work)

## Overview

**Features**

- Length-prefixed binary protocol, version 2, with 11 message types.
- Unique, case-insensitive usernames (1-16 characters from `A-Z a-z 0-9 _ -`).
- Public chat, private messages (`/msg`), online list (`/users`).
- Styled terminal client: timestamp column, per-user colours, word wrapping, a
  one-row line editor with history, UTF-8 aware. It falls back to a plain
  line-by-line format when stdin or stdout is not a terminal (pipes, scripts).
- Server with graceful shutdown on `Ctrl+C`/`SIGTERM`, handshake and send
  timeouts, a connection limit, reaping of finished threads, a start-up banner
  and timestamped, coloured log lines (colour only on a terminal).
- Untrusted text is sanitised on both sides, so a peer cannot inject terminal
  escape sequences.

**What changed from the ncat v1**

The earlier version used a bash-based `ncat` setup: `ncat -l --broker` acted
as the server, while each client piped lines through `ncat host port`. `ncat`
simply copied raw bytes between all connected clients. Messages were prefixed
with `[HH:MM] [USER]:` *on the client*, so the server had no idea who anyone
was, could not validate anything, and any client could claim any name. That
implementation is preserved in the `legacy-bash` branch. The current version
replaces that approach with our own C++ server and client:

| | v1 (`legacy-bash` branch) | v2 (this repo) |
|---|---|---|
| Server | `ncat --broker` (external tool) | `termichat_server`, our own accept loop and threads |
| Message boundaries | none (raw bytes) | 4-byte length prefix |
| Sender name | typed by the client in its own text | assigned by the server, unique, validated |
| Private messages, user list | no | yes |
| Timeouts, limits, graceful shutdown | no | yes |

## Requirements and install

- Linux, a C++17 compiler (g++), `make`, POSIX threads. No other libraries.
- A UTF-8 locale is optional: without it the client uses ASCII symbols.
- Optional tools for packet inspection: `tcpdump`, Wireshark, and `iproute2` (`ss`).

```sh
# Debian / Ubuntu
sudo apt install build-essential

# Arch
sudo pacman -S base-devel

# Fedora
sudo dnf install gcc-c++ make
```

## Build, run, flags and commands

```sh
make            # builds ./termichat_server and ./termichat_client
make clean
```

Start the server, then clients (in other terminals, or on other machines):

```sh
./termichat_server 12345
./termichat_client localhost 12345 alice
```

### Command-line flags

| Program | Usage | Notes |
|---|---|---|
| `termichat_server` | `termichat_server [port]` | `port` 1-65535, default `12345`. `-h`/`--help` prints usage and exits 0. More than one argument, or a bad port: usage, exit 2. |
| `termichat_client` | `termichat_client [host] [port] [username]` | `host` default `localhost`, `port` default `12345`. `-h`/`--help` (as the first argument) prints usage, exit 0. If `username` is omitted and stdin is a terminal, it is asked for (3 tries). If it is omitted and stdin is not a terminal: error, exit 2. |

### In-chat commands

A line starting with `/` is always a command and is never sent as chat text.
Chat lines are limited to 1024 bytes.

| Command | Effect |
|---|---|
| `/help` | Show the command list. |
| `/users` | List who is online (asks the server with a `LIST_REQ`). |
| `/msg <user> <text>` | Private message to one user. You get a copy back as confirmation. |
| `/clear` | Clear the screen. Interactive mode only; in plain mode it is an unknown command. |
| `/quit` | Leave. `Ctrl+D` on an empty line, `Ctrl+C` and `SIGTERM` do the same. |

Editor keys (interactive mode): arrows, `Home`/`End` (`Ctrl+A`/`Ctrl+E`),
`Delete`, `Backspace`, `Ctrl+U` (clear line), `Ctrl+W` (delete word),
`Ctrl+L` (redraw), `Up`/`Down` (history of the last 50 lines).

### Environment variables

| Variable | Used by | Meaning | Default / range |
|---|---|---|---|
| `NO_COLOR` | both | If set and non-empty: no ANSI colours (client UI, server log tags and banner). | unset |
| `TERM` | client | `TERM=dumb` (or unset) selects plain mode. | n/a |
| `TERMICHAT_MAX_CLIENTS` | server | **Test-only.** Connection limit, joined or not. | `64`, 1-1024 |
| `TERMICHAT_HANDSHAKE_TIMEOUT_S` | server | **Test-only.** Seconds of silence allowed before JOIN completes. | `10`, 1-3600 |
| `TERMICHAT_SEND_TIMEOUT_S` | server | **Test-only.** Longest a `send()` to one client may block. | `5`, 1-3600 |

An invalid `TERMICHAT_*` value is ignored (the default is used) and a `WARN`
line is logged. The 2-second quit drain of the client is a constant, not a knob.

### Output formats

*Server log* (stdout): `HH:MM:SS  LEVEL  message`, one line per event, flushed
per line. `INFO` is cyan, `WARN` yellow, `ERROR` red, only on a terminal with
`NO_COLOR` unset. The start-up banner (title, port, the machine's LAN IPv4
addresses via `getifaddrs`, "Press Ctrl+C to stop") is printed only on a
terminal. The line `Listening on 0.0.0.0:PORT` is always logged.

*Plain client output* (not a terminal): `[HH:MM] alice: hi`,
`[HH:MM] [private] alice -> bob: text`, `*** notice text`,
`*** Online (N): a, b`.

## Architecture

```
  client A --+
  client B --+--  TCP  -->  [ accept loop ]  --spawn-->  [ per-client thread ]
  client C --+            poll() 200 ms, limit            recv_frame loop
                          reaps finished threads                 |
                                                                 v
        +----------------------+   register / unregister / snapshot
        |  registry            | <------------------------------+
        |  name -> session     |
        |  (registry_mutex_)   | ---- copy of recipient list --->  broadcast
        +----------------------+                                     |
                                                                     v
                             send_frame() on each recipient's socket
                             (one send_mutex_ per session, send timeout)
```

### File map

| Path | Purpose |
|---|---|
| `src/server_main.cpp` | Server entry point: argument parsing, top-level error handling. |
| `src/server.hpp`, `src/server.cpp` | `Server` (accept loop, tracked threads, registry, shutdown, limits, logging, banner) and `ClientSession`. |
| `src/client_main.cpp` | Client entry point: argument parsing, username prompt. |
| `src/client.hpp`, `src/client.cpp` | `Client` (connect, join, receiver thread, stdin loop, graceful quit) and the `Screen` class (the only code that writes chat output). |
| `src/protocol.hpp`, `src/protocol.cpp` | Message types, frame send/receive, payload encoders/decoders, `ChatEvent`. |
| `src/socket_utils.hpp`, `src/socket_utils.cpp` | `Socket` (RAII file descriptor), `NetError`, `send_all`, `recv_all`. |
| `src/text_utils.hpp`, `src/text_utils.cpp` | Username rules, `sanitize_text`, small helpers (`unix_now`, `format_clock`, ...). |
| `src/terminal.hpp`, `src/terminal.cpp` | Terminal detection, raw mode, signal handlers, key decoder, `write_all`. |
| `src/line_editor.hpp`, `src/line_editor.cpp` | The line being typed: UTF-8 buffer, cursor, history. |
| `src/render.hpp`, `src/render.cpp` | Pure functions turning events into terminal lines (wrapping, banner, help, input row). |
| `Makefile` | `all`, `test`, `clean`. |
| `tests/` | Unit tests and `smoke_test.sh`, as referenced by the Makefile. |

## Networking explained

### Server: system calls in order

1. `sigaction(SIGINT / SIGTERM)` without `SA_RESTART`: the handler only stores
   the signal number in a lock-free atomic. `SIGPIPE` is set to ignore.
2. `socket(AF_INET, SOCK_STREAM, 0)`: create an IPv4 TCP endpoint.
3. `setsockopt(SO_REUSEADDR)`: allow a quick restart (see TIME_WAIT below).
4. `bind()` to `INADDR_ANY:port`: attach the endpoint to all interfaces.
5. `listen(fd, 16)`: switch to the passive state; the kernel now completes
   handshakes by itself and queues up to 16 finished connections.
6. `open("/dev/null")`: one spare descriptor, used only to survive `EMFILE`.
7. Banner (terminal only, uses `getifaddrs`) and the `Listening on 0.0.0.0:PORT` log line.
8. Loop: `poll()` on the listening socket with a 200 ms timeout, so the stop flag
   is checked even if the signal landed on another thread. Finished client
   threads are joined here.
9. `accept()`: take one already-established connection; it returns a new fd.
   `inet_ntop()` formats the peer address for the log.
10. `setsockopt(SO_SNDTIMEO)` on every accepted socket and `SO_RCVTIMEO` for the
    handshake phase.
11. If the connection limit is reached: send `JOIN_ERR "Server is full"`,
    `shutdown(SHUT_RDWR)`, close; no thread is started.
12. Otherwise start one `std::thread` for the client (on Linux a thin wrapper
    over POSIX threads).
13. In that thread: `recv()` in a loop (`recv_all`: 4-byte header, then body),
    handle the frame, `send(..., MSG_NOSIGNAL)` for replies.
14. When the connection ends: `shutdown(SHUT_RDWR)`; the fd is `close()`d by the
    `Socket` destructor when the last `shared_ptr` to the session disappears.
15. On `SIGINT`/`SIGTERM`: close the listening socket, send the shutdown notice,
    `shutdown()` every connection, join every thread (see
    [Concurrency design](#concurrency-design)).

### Client: system calls in order

1. `getaddrinfo()` (IPv4, TCP): name resolution.
2. For each result: `socket()` then `connect()` until one succeeds. `connect()`
   blocks until the three-way handshake is complete.
3. `send()` a `JOIN` frame, then a blocking `recv()` for `JOIN_OK` or `JOIN_ERR`.
   (The client has no timeout here; `Ctrl+C` during this wait uses the default
   signal action, because the client's handlers are installed after joining.)
4. After `JOIN_OK`: `sigaction()` for `SIGINT`, `SIGTERM`, `SIGHUP`, `SIGWINCH`
   (flags only, no `SA_RESTART`), and, on a terminal, `tcsetattr()` for raw mode.
5. A receiver thread loops on `recv()` and hands events to the `Screen`.
6. The main thread loops on `poll()` of stdin (200 ms), `read()`s, and `send()`s.
7. Quit: `send(QUIT)`, `shutdown(SHUT_WR)`, wait up to 2 s for the server to
   close, `shutdown(SHUT_RDWR)`, join the receiver, `close()` via the destructor.

### Concepts in plain language

**Three-way handshake vs `connect()` / `accept()`.** TCP opens a connection with
SYN, SYN-ACK, ACK. The *client's* `connect()` sends the SYN and returns once the
SYN-ACK has arrived and the final ACK was sent. On the server the *kernel*
answers the SYN and completes the handshake on its own; `accept()` does not
take part in it, it only removes an already-established connection from the
queue and gives the program a file descriptor for it.

**`listen()` backlog.** The backlog (16 here) bounds that queue of completed,
not-yet-accepted connections. A server that is slow to call `accept()` lets the
queue fill; what the kernel then does with new connection attempts is
kernel-dependent (they are typically delayed or dropped and the client retries).

**TCP is a byte stream, not a message stream.** One `send()` does not equal one
`recv()`: TCP may merge two sends into one segment or split one send over several,
and `recv()` returns whatever has arrived. The application must therefore mark
message boundaries itself.

**Length-prefix framing, and why newline framing is fragile.** Every frame starts
with a 4-byte length, then a type byte, then the payload. The reader knows
exactly how many bytes to wait for, and `recv_frame` rejects an impossible
length *before* allocating memory. Newline framing breaks when the data may
contain the delimiter (it needs escaping), cannot bound the size of a line
before reading it, and cannot carry arbitrary binary payloads.

**Partial reads and writes.** `recv()` may return fewer bytes than asked and
`send()` may accept fewer than offered; either can also be interrupted by a signal
(`EINTR`). `recv_all` and `send_all` loop until all bytes have moved, retrying on
`EINTR`. `recv_all` also reports *how many* bytes arrived before the peer closed,
which separates a clean close between frames from a truncated frame.

**Network byte order.** The length field is sent big-endian (`htonl` /
`ntohl`), so machines with different endianness agree. Everything else in the
payload is text (timestamps are decimal digits), so no other byte-order
conversion exists.

**Blocking I/O, timeouts and backpressure.** All sockets are blocking. Without
limits, one client that stops reading would eventually fill its kernel buffers
and make the server's `send()` to it block; the thread relaying a message would
be stuck there while holding that client's send lock, and everyone else's
messages to the same client would queue behind it. `SO_SNDTIMEO` (5 s) turns
that into an error after a bounded time and only that client is dropped.
`SO_RCVTIMEO` (10 s) keeps a connection that never sends `JOIN` from holding a
thread and a slot for ever; it is cleared when the handshake succeeds, so joined
users may stay silent.

**`SIGPIPE` and `MSG_NOSIGNAL`.** Writing to a socket whose peer has gone away
raises `SIGPIPE`, whose default action kills the process. Every `send()` passes
`MSG_NOSIGNAL` and the server also ignores `SIGPIPE`, so the failure arrives as
the error `EPIPE`, which is handled like any other send failure.

**`SO_REUSEADDR` and `TIME_WAIT`.** The side that closes a connection first
(the *active closer*) keeps the connection in `TIME_WAIT` for a while (60 s on
Linux) so that late packets cannot be confused with a new connection. If the
server was the active closer, a restart would find local port 12345 still
occupied by those sockets and `bind()` would fail with `EADDRINUSE`.
`SO_REUSEADDR` allows the bind anyway.

**`shutdown()` vs `close()`; half-close.** `close()` releases the descriptor.
`shutdown(fd, how)` acts on the *connection* and leaves the descriptor open:
`SHUT_WR` sends our FIN (we will send no more) but we can still read;
`SHUT_RDWR` closes both directions and wakes any thread blocked in `recv()` or
`send()` on that socket. The server uses `SHUT_RDWR` to wake threads at shutdown
(other threads still hold the descriptor, so it is closed later by the RAII
`Socket`). The client uses the half-close on quit: send `QUIT`, send our FIN, read
until the server's FIN.

**FIN vs RST.** FIN is the orderly close: "I have no more data to send"; each
direction ends separately. RST is an abort: the connection is torn down at once
and unread data may be lost. A RST is sent, for example, when a socket is closed
while unread data is still in its receive queue, when data arrives for a port
nobody listens on, or when `SO_LINGER` is set to `{1, 0}` before `close()`. That
is why the client does not simply `close()` after `QUIT`: it reads until the
server's FIN first so that the close is a FIN, not a RST.

## Protocol

Frame: `| 4-byte big-endian length | 1-byte type | payload |`, where the length
counts the type byte plus the payload. Payload at most 2048 bytes.

| # | Type | Direction | Payload |
|---|---|---|---|
| 1 | `JOIN` | C to S | username |
| 2 | `JOIN_OK` | S to C | username |
| 3 | `JOIN_ERR` | S to C | reason (connection is closed afterwards) |
| 4 | `SAY` | C to S | chat text |
| 5 | `MSG` | S to C | `<unix_ts>\|<sender>\|<text>` |
| 6 | `SYSTEM` | S to C | `<unix_ts>\|<kind>\|<text>` (kind: info, join, leave, error, shutdown) |
| 7 | `LIST_REQ` | C to S | empty |
| 8 | `LIST_RES` | S to C | `name1,name2,...` |
| 9 | `QUIT` | C to S | empty |
| 10 | `PRIV_SAY` | C to S | `<target>\|<text>` |
| 11 | `PRIV_MSG` | S to C | `<unix_ts>\|<from>\|<to>\|<text>` |

The protocol implementation is in `src/protocol.hpp` and `src/protocol.cpp`.
Use these files when inspecting or extending message types and payload encoding.
Keep the client and server handling consistent when changing the protocol, then
run `make test` to exercise the framing and protocol tests.

## Concurrency design

**Threads.** The accept thread (the one running `Server::run`) accepts and
reaps; it also performs the shutdown. Each connection has one tracked
`std::thread` running the `recv_frame` loop. The client has a receiver thread and
the main thread (stdin loop). There are no detached threads.

**What each lock protects**

| Lock | Protects | Held during I/O? |
|---|---|---|
| `registry_mutex_` | `clients_`, the map from lower-cased name to session | Never. Only map operations. |
| `ClientSession::send_mutex_` (one per session, a `timed_mutex`) | Writes on that session's socket, so frames never interleave | Yes: it is held during `send()`, which the send timeout bounds. |
| `g_log_mutex` | The log stream | Held only while printing one line. |
| `connections_` list | Nothing: only the accept thread touches it | n/a |
| atomics (`stopping_`, `peak_users_`, `relayed_`, the stop signal) | Flags and counters | n/a |
| `Screen` mutex (client) | All terminal output, so a redraw never tears a line | Yes, while writing to the terminal. |

**Lock order**, outermost first: `send_mutex_` then `registry_mutex_` then
`g_log_mutex`. Only `handle_join` holds two at once (the new session's
`send_mutex_`, then briefly the registry); everything else holds at most one,
and no thread ever holds two `send_mutex_`s.

**Why broadcast takes a snapshot.** To send to everyone, the code copies the
list of `shared_ptr<ClientSession>` under the registry lock, releases the lock,
and only then sends. If the lock were held during `send()`, one slow client would
block every join, leave and lookup. The `shared_ptr` copies also keep each
socket's descriptor alive while it is written to, so a client that leaves at that
moment cannot cause a write to a recycled descriptor. The price: a message may
reach a user who left a microsecond earlier; the send then fails quietly.

**Graceful shutdown sequence** (`SIGINT` or `SIGTERM`; a second signal exits at once):

1. The handler stores the signal; the accept loop sees it within 200 ms.
2. `stopping_` is set, so client threads stop logging and sending leave notices.
3. The listening socket is closed.
4. `SYSTEM(kind=shutdown, "Server is shutting down")` goes to every joined user
   with `MSG_DONTWAIT` and one shared 300 ms deadline, so a stalled client
   cannot delay it (a client that is skipped just sees the connection close).
5. `shutdown(SHUT_RDWR)` on every connection, joined or not; this wakes every
   blocked `recv()` and `send()`.
6. Every client thread is joined.
7. A summary is logged (uptime, peak users, messages relayed) and the server exits 0.

## Error handling and exit codes

| Situation | Behaviour |
|---|---|
| Bad command-line arguments | Usage text on stderr, exit 2. |
| Client cannot connect | `Error: connect() to host:port: ...`, exit 1. |
| Invalid username, name taken, server full | Server sends `JOIN_ERR` and closes; the client prints `Error: <reason>` and exits 1. |
| Client silent for the handshake timeout | Server logs `rejected: handshake timeout` and drops it. |
| Client does not read (send blocks for the send timeout) | Server logs `Send to <name> failed: send timeout` and drops that client only. |
| Malformed frame, unknown type, wrong type for the state, over-long text | Server drops the connection (no reply). Joined users are announced as `<name> lost connection`. |
| Message too long, unknown `/command`, bad `/msg` use | Local notice in the client; nothing is sent. |
| `/msg` to yourself, unknown user, empty text | Server replies with a `SYSTEM` frame of kind `error`. |
| Server vanishes without notice | Client prints `Connection lost (...)`, exit 1. |
| Server announced its shutdown, then closed | Client prints `Server shut down`, exit 0. |
| `accept()` runs out of descriptors (`EMFILE`) | One connection is dropped, `ERROR` logged, accept loop continues. |
| Thread cannot be started | `WARN` logged, only that client is dropped. |
| Peer disappears during a write | `EPIPE` instead of a fatal `SIGPIPE`. |

**Exit codes**

| Code | Server | Client |
|---|---|---|
| `0` | Stopped after `SIGINT`/`SIGTERM`; `--help`. | User quit (`/quit`, `Ctrl+D`, `Ctrl+C`, `SIGTERM`, end of piped input), `Server shut down`; `--help`. |
| `1` | Fatal setup error (`Fatal: ...`: socket, bind, listen...). | Server gone or network error, connection or join refused. |
| `2` | Bad usage. | Bad usage (port, username, argument count). |
| `128+signo` | A second signal during shutdown exits immediately (130 for `SIGINT`, 143 for `SIGTERM`). | n/a |

## Testing

```sh
make test       # builds everything, runs the unit tests, then the smoke test
```

`make test` (see the `Makefile`) builds both programs, builds and runs four unit
test programs (`test_framing`, `test_protocol`, `test_text`, `test_ui`, from
`tests/`), and then runs `bash tests/smoke_test.sh 3`, the multi-client scenario
script, three times in a row. The unit tests cover the framing layer, the message
codecs, the text and username helpers, and the UI renderer; read
`tests/smoke_test.sh` for the exact multi-client scenarios.

The Makefile has no `test-asan` or `test-tsan` targets. To run the same suite
under a sanitizer, override `CXXFLAGS` and rebuild from scratch (the objects live
in `build/`, so clean between modes):

```sh
# AddressSanitizer + UndefinedBehaviorSanitizer
make clean
make CXXFLAGS='-std=c++17 -Wall -Wextra -g -O1 -pthread -fno-omit-frame-pointer -fsanitize=address,undefined' LDFLAGS='-pthread -fsanitize=address,undefined' test

# ThreadSanitizer (cannot be combined with AddressSanitizer)
make clean
make CXXFLAGS='-std=c++17 -Wall -Wextra -g -O1 -pthread -fsanitize=thread' LDFLAGS='-pthread -fsanitize=thread' test
make clean
```

## Packet analysis with Wireshark

Wireshark is an external tool, not part of TermiChat. To inspect TermiChat traffic,
start the server and connect one or more clients, then capture on the loopback
interface (for local clients) or the network interface used by remote clients.
Use the display filter `tcp.port == 12345` (replace the port if you use a
different one), send public and private messages, request `/users`, and then
disconnect. Inspect the TCP payload or use **Follow TCP Stream** to view the
ordered byte stream. Since TCP does not preserve application message boundaries,
a packet may contain part of a TermiChat frame or several frames; use the
4-byte length prefix and 1-byte message type described above to interpret it.
Wireshark displays this as a custom protocol carried over TCP, rather than
automatically decoding TermiChat's message names.

## Limitations and future work

- **No encryption and no authentication.** Traffic is plaintext; anyone who can
  reach the port can take any free username. TLS and accounts would be the next step.
- **IPv4 only** (the server binds `AF_INET`, the client resolves with `AF_INET`).
- **Thread per client**, capped at 64 connections by default. An `epoll`-based
  event loop would scale further but is a bigger design change.
- **No message history or persistence**; a user who joins sees only new messages.
- **No rate limiting** of how fast a client may send.
- The handshake timeout is a per-`recv()` inactivity timeout, not an absolute
  deadline for the whole handshake.
- The client has no timeout while waiting for `JOIN_OK`.
- `TCP_NODELAY` is not set, so small frames can be delayed by Nagle's algorithm.
- The server logs to stdout only.