# TermiChat (Phase 1: Working Multi-Client TCP Chat)

TermiChat is a lightweight, terminal-based multi-client chat system written in C++17 using raw POSIX sockets and POSIX threads (`pthread`). It demonstrates core computer networking concepts: reliable TCP stream framing, non-interleaved multi-client broadcasting, concurrent socket I/O without global lock contention, and robust error recovery.

---

## Evolution: C++17 POSIX Sockets vs. the Old `ncat` Script

The original TermiChat prototype (`termichat.sh`) relied entirely on external wrapper scripts around `ncat --broker`. While functional as a quick prototype, relying on `ncat` hid all transport-layer reality:
1. **No Application Framing:** `ncat` treats data as unbounded line-delimited streams; it cannot differentiate application-layer message boundaries from arbitrary TCP buffer segmentations.
2. **External Dependency:** Required third-party tools (`nmap` / `ncat`) installed on the host.
3. **No Protocol Extensibility:** Cannot accommodate binary headers, typed packets, or secure state machines.
4. **No Direct Concurrency Control:** Client session registries, synchronization, and connection lifecycles were black boxes.

This Phase 1 implementation replaces the prototype with native C++17 systems programming: direct invocation of POSIX Berkeley sockets (`socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv`, `shutdown`), explicit length-prefixed binary framing, RAII resource management, and fine-grained mutex synchronization.

---

## Requirements & Installation

A C++17 compliant compiler (`g++` >= 9) and GNU `make` are required.

### Debian / Ubuntu
```bash
sudo apt update
sudo apt install g++ make iproute2
```

### Arch Linux / Manjaro
```bash
sudo pacman -S gcc make iproute2
```

### Fedora / RHEL
```bash
sudo dnf install gcc-c++ make iproute
```

---

## Building and Running

### Build Targets
```bash
make clean && make    # Compiles server and client binaries with -O2 -Wall -Wextra -Wpedantic
make test             # Executes test_framing unit tests and smoke_test.sh (3 consecutive runs)
```

Binaries generated in the project root:
- `./termichat_server [port]` (Default: `12345`)
- `./termichat_client [host] [port]` (Defaults: `localhost`, `12345`)

Both binaries support `-h` / `--help` and strictly validate port ranges (1–65535 via `strtol`). Bad arguments exit with status code `2`.

### Finding Your LAN IP & Firewall Configuration
To allow other devices on your local network to connect:
1. Find your interface IP address:
   ```bash
   ip -4 addr
   ```
   Look for `inet` under your active network adapter (e.g., `192.168.1.50`).
2. If a firewall is active, allow incoming TCP connections on the chat port:
   ```bash
   sudo ufw allow 12345/tcp      # Ubuntu / Debian UFW
   sudo firewall-cmd --add-port=12345/tcp --permanent && sudo firewall-cmd --reload # Fedora firewalld
   ```

---

## Wire Protocol & Framing

TCP is a continuous byte stream with no intrinsic message boundaries. A single `send()` on the sender does not map to a single `recv()` on the receiver: network packets may be fragmented into smaller chunks or coalesced into larger buffers.

TermiChat enforces framing with a 5-byte header prefix on every message:
```
+-----------------------------------+---------------+-----------------------+
|  4 Bytes: Length (Big-Endian)     | 1 Byte: Type  | Payload Bytes         |
+-----------------------------------+---------------+-----------------------+
```
- **Length (32-bit uint in network byte order / `htonl`):** Counts `1 (Type byte) + Payload Length`.
- **Type (8-bit uint):** Message discriminator. Phase 1 defines `CHAT = 1`.
- **Payload:** Raw UTF-8 bytes (binary-safe, un-terminated).
- **Constants:** `MAX_PAYLOAD = 2048`, `MAX_CHAT_TEXT = 1024`.

### Real Wire Byte Inspection
From unit test verification (`test_framing.cpp`), sending a `CHAT` message with payload `"hi"` produces exactly:
```
00 00 00 03 01 68 69
```
- `00 00 00 03`: 4-byte big-endian integer indicating body length = 3 bytes
- `01`: Type byte (`MsgType::CHAT = 1`)
- `68 69`: ASCII bytes for `'h'` (`0x68`) and `'i'` (`0x69`)

Before allocating memory for incoming payloads, `recv_frame` inspects the 4-byte header and rejects lengths equal to `0` or greater than `1 + MAX_PAYLOAD` with `FrameStatus::PROTOCOL_VIOLATION`, protecting the server from denial-of-service allocations.

---

## Syscall Sequences & Networking Lifecycle

### Server Syscall Sequence
1. **`socket(AF_INET, SOCK_STREAM, 0)`**: Creates an IPv4 TCP communication endpoint in the kernel.
2. **`setsockopt(..., SO_REUSEADDR, ...)`**: Informs the kernel that the local port can be rebound immediately, even if previous sockets remain in `TIME_WAIT` state.
3. **`bind(...)`**: Binds the socket to `INADDR_ANY` (`0.0.0.0`) and the requested port in network byte order (`htons`).
4. **`listen(..., 16)`**: Marks the socket as passive, ready to accept incoming connection requests with a listen backlog queue size of 16.
5. **`accept(...)`**: Dequeues a fully established connection from the kernel's completed connection queue (connections that have completed the TCP three-way handshake). Returns a distinct socket file descriptor dedicated to that client.
6. **`recv(...)` / `send(...)`**: Data transfer. Server loops on `recv()` with `recv_frame()`.
7. **`shutdown(..., SHUT_RDWR)`**: Used when dropping dead or errored connections to wake client threads from blocking system calls without closing file descriptors out from under other threads.

### Client Syscall Sequence
1. **`getaddrinfo(...)`**: Resolves hostnames or IP strings into candidate `struct addrinfo` records safely and in a thread-safe manner.
2. **`socket(...)`**: Creates the client TCP endpoint.
3. **`connect(...)`**: Initiates the TCP three-way handshake (`SYN` -> `SYN-ACK` -> `ACK`). Blocks until the kernel completes the handshake or times out/is refused.
4. **`poll(..., STDIN_FILENO, 200ms)` + `read(...)`**: Non-blocking input loop monitoring user terminal input without stream buffering issues.
5. **`send(..., MSG_NOSIGNAL)`**: Sends frames. `MSG_NOSIGNAL` prevents the operating system from terminating the process with `SIGPIPE` if the remote server disconnects unexpectedly; instead, the call returns `-1` with `errno = EPIPE`.

---

## File Layout

```
.
├── Makefile                     # Build system with dependency tracking (-MMD -MP)
├── README.md                    # Project documentation and networking guide
├── src/
│   ├── socket_utils.hpp / .cpp  # RAII Socket, NetError, send_all, recv_all
│   ├── protocol.hpp / .cpp      # Wire constants, send_frame, recv_frame
│   ├── server.hpp / .cpp        # Server engine, ClientSession, broadcast logic
│   ├── server_main.cpp          # Server CLI entry point and argument parsing
│   ├── client.hpp / .cpp        # Client engine, polling stdin, background receiver
│   └── client_main.cpp          # Client CLI entry point and argument parsing
└── tests/
    ├── test_framing.cpp         # Unit tests (round-trip, fragmentation, coalescing, DoS length rejection)
    └── smoke_test.sh            # Multi-client automated test runner (real binaries, FIFOs)
```

---

## Real Session Transcript & Socket Inspection

Captured from an actual 3-client terminal session:

### Socket States (`ss`)
```
$ ss -tlnp sport = :45678
State  Recv-Q Send-Q Local Address:Port  Peer Address:Port Process
LISTEN 0      16           0.0.0.0:45678      0.0.0.0:*    users:(("termichat_serve",pid=401,fd=3))

$ ss -tnp ( sport = :45678 or dport = :45678 )
State Recv-Q Send-Q Local Address:Port  Peer Address:Port Process                                  
ESTAB 0      0          127.0.0.1:33008    127.0.0.1:45678 users:(("termichat_clien",pid=552,fd=3))
ESTAB 0      0          127.0.0.1:33030    127.0.0.1:45678 users:(("termichat_clien",pid=562,fd=3))
ESTAB 0      0          127.0.0.1:45678    127.0.0.1:33022 users:(("termichat_serve",pid=401,fd=5))
ESTAB 0      0          127.0.0.1:45678    127.0.0.1:33008 users:(("termichat_serve",pid=401,fd=4))
ESTAB 0      0          127.0.0.1:33022    127.0.0.1:45678 users:(("termichat_clien",pid=557,fd=3))
ESTAB 0      0          127.0.0.1:45678    127.0.0.1:33030 users:(("termichat_serve",pid=401,fd=6))
```

### Server Log
```
[03:22:48] [INFO] Listening on 0.0.0.0:45678
[03:22:48] [INFO] Client 1 connected from 127.0.0.1:33008 (1 online)
[03:22:48] [INFO] Client 2 connected from 127.0.0.1:33022 (2 online)
[03:22:49] [INFO] Client 3 connected from 127.0.0.1:33030 (3 online)
[03:22:49] [INFO] Client 3 disconnected (peer closed connection) (2 online)
```

### Client Output
Client 1 types `"Hello from Student Alice"`. Client 1 receives no self-echo:
```
Connected to localhost:45678
```

Client 2 output receives the broadcast:
```
Connected to localhost:45678
[03:22] [Client 1]: Hello from Student Alice
[03:22] [Client 1]: Server survived Client 3 abrupt exit!
```

---

## Exit Codes

- `0`: Normal clean exit (user ended input with `Ctrl+D`).
- `1`: Server closed connection, connection refused, or unexpected I/O / protocol failure.
- `2`: Invalid CLI arguments or port range out of bounds.

---

## Known Limitations (Phase 1 Scope)

By design for Phase 1, the following are intentionally deferred to future phases:
- No user-defined nicknames (server assigns `Client 1`, `Client 2`, etc.).
- No join/leave broadcast notices.
- No slash commands (`/nick`, `/pm`, `/quit`).
- Terminal lines may interleave with incoming messages while the user is actively typing (no raw-mode ncurses/readline UI).
- Per-client server threads are detached; graceful server shutdown with tracked joinable threads is scheduled for Phase 4.

---

## 5-Phase Roadmap

- **Phase 1: Working Multi-Client TCP Chat with Framing** — **[DONE]**
- **Phase 2:** Protocol Handshake, Usernames, and Join/Leave Notices
- **Phase 3:** Slash Commands & Private Messaging (`/pm`)
- **Phase 4:** Graceful Shutdown, Signal Handling & Hardening
- **Phase 5:** Final Integration Testing, Wireshark Packet Captures & Viva Documentation
