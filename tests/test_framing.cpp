// tests/test_framing.cpp – unit tests for the framing layer.
// Plain checks, no external framework.  Uses socketpair() so no network or
// server process is needed.  The wire format is checked against frames built
// by hand here, independently of send_frame(), so an encoder bug cannot hide
// behind a matching decoder bug.

#include "../src/socket_utils.hpp"
#include "../src/protocol.hpp"

#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// ── Tiny check helper ────────────────────────────────────────────────────────
static int g_pass = 0, g_fail = 0;

static void check(bool cond, const char* label) {
    std::printf("  %s: %s\n", cond ? "PASS" : "FAIL", label);
    cond ? ++g_pass : ++g_fail;
}

// ── Helpers ──────────────────────────────────────────────────────────────────
struct Pair {
    Socket a, b;   // a = writer side, b = reader side
    Pair() : a(-1), b(-1) {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            std::perror("socketpair");
            std::exit(2);
        }
        a = Socket(sv[0]);
        b = Socket(sv[1]);
    }
};

// Builds a frame by hand: | big-endian length | type | payload |
static std::string raw_frame(uint32_t length, uint8_t type, const std::string& payload) {
    std::string f(4, '\0');
    const uint32_t n = htonl(length);
    std::memcpy(f.data(), &n, 4);
    f += static_cast<char>(type);
    f += payload;
    return f;
}

// A well-formed CHAT frame for 'payload'.
static std::string good_frame(const std::string& payload) {
    return raw_frame(1 + static_cast<uint32_t>(payload.size()), 1, payload);
}

static void write_raw(Socket& s, const std::string& bytes) {
    send_all(s.fd(), bytes.data(), bytes.size());
}

static FrameStatus read_one(Socket& s, MsgType& t, std::string& p) {
    return recv_frame(s.fd(), t, p);
}

// ── Tests ────────────────────────────────────────────────────────────────────
static void test_round_trip() {
    std::puts("Test 1: encode/decode round trip");
    Pair p;
    MsgType t; std::string out;

    check(send_frame(p.a.fd(), MsgType::CHAT, "hello") == FrameStatus::OK, "send_frame returns OK");
    check(read_one(p.b, t, out) == FrameStatus::OK, "recv_frame returns OK");
    check(t == MsgType::CHAT && out == "hello", "type and payload match");

    const std::string binary("a\0b\xff\xc3\xa9", 6);   // NUL and non-ASCII bytes
    send_frame(p.a.fd(), MsgType::CHAT, binary);
    check(read_one(p.b, t, out) == FrameStatus::OK && out == binary,
          "payload with NUL / non-ASCII bytes survives unchanged");

    send_frame(p.a.fd(), MsgType::CHAT, "");
    check(read_one(p.b, t, out) == FrameStatus::OK && out.empty(), "empty payload is a valid frame");
}

static void test_header_bytes() {
    std::puts("Test 2: wire format is big-endian: 00 00 00 03 01 68 69");
    Pair p;
    send_frame(p.a.fd(), MsgType::CHAT, "hi");
    unsigned char buf[7] = {};
    check(recv_all(p.b.fd(), buf, 7) == IOStatus::OK, "7 bytes on the wire (4 length + 1 type + 2 payload)");
    const unsigned char want[7] = {0x00, 0x00, 0x00, 0x03, 0x01, 'h', 'i'};
    check(std::memcmp(buf, want, 7) == 0, "bytes are exactly 00 00 00 03 01 68 69");
}

static void test_fragmentation() {
    std::puts("Test 3: fragmentation (one byte at a time, with delays)");
    Pair p;
    const std::string frame = good_frame("fragmented message");
    std::thread w([&] {
        for (char c : frame) {
            ::send(p.a.fd(), &c, 1, MSG_NOSIGNAL);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    MsgType t; std::string out;
    check(read_one(p.b, t, out) == FrameStatus::OK, "one complete frame is reassembled");
    check(out == "fragmented message", "payload is intact");
    w.join();
}

static void test_coalescing() {
    std::puts("Test 4: coalescing (two frames in a single send)");
    Pair p;
    write_raw(p.a, good_frame("first") + good_frame("second"));   // one send_all()
    MsgType t; std::string out;
    check(read_one(p.b, t, out) == FrameStatus::OK && out == "first", "first frame is separate");
    check(read_one(p.b, t, out) == FrameStatus::OK && out == "second", "second frame is separate");
}

static void test_invalid_lengths() {
    std::puts("Test 5: invalid length fields are rejected before reading/allocating");
    MsgType t; std::string out;
    {
        Pair p; write_raw(p.a, std::string(4, '\0'));
        check(read_one(p.b, t, out) == FrameStatus::PROTOCOL_VIOLATION, "length 0 -> PROTOCOL_VIOLATION");
    }
    {
        Pair p; write_raw(p.a, std::string(4, '\xff'));
        check(read_one(p.b, t, out) == FrameStatus::PROTOCOL_VIOLATION, "length 0xFFFFFFFF -> PROTOCOL_VIOLATION");
    }
    {
        Pair p; write_raw(p.a, raw_frame(1 + MAX_PAYLOAD + 1, 1, "").substr(0, 4));
        check(read_one(p.b, t, out) == FrameStatus::PROTOCOL_VIOLATION, "length one past the limit -> PROTOCOL_VIOLATION");
    }
    {
        Pair p; write_raw(p.a, good_frame(std::string(MAX_PAYLOAD, 'x')));
        check(read_one(p.b, t, out) == FrameStatus::OK && out.size() == MAX_PAYLOAD, "length exactly at the limit is accepted");
    }
}

static void test_unknown_type() {
    std::puts("Test 6: unknown message type");
    Pair p; MsgType t; std::string out;
    write_raw(p.a, raw_frame(2, 0x7f, "x"));
    check(read_one(p.b, t, out) == FrameStatus::PROTOCOL_VIOLATION, "type 0x7f -> PROTOCOL_VIOLATION");
}

static void test_closes() {
    std::puts("Test 7: clean close vs truncated frame");
    MsgType t; std::string out;
    {   // FIN with nothing sent: normal end of conversation
        Pair p; p.a = Socket(-1);
        check(read_one(p.b, t, out) == FrameStatus::PEER_CLOSED, "FIN between frames -> PEER_CLOSED");
    }
    {   // one full frame, then FIN
        Pair p; write_raw(p.a, good_frame("bye")); p.a = Socket(-1);
        check(read_one(p.b, t, out) == FrameStatus::OK, "frame before the FIN is still delivered");
        check(read_one(p.b, t, out) == FrameStatus::PEER_CLOSED, "then FIN -> PEER_CLOSED");
    }
    {   // header promises 10 bytes, only 3 arrive
        Pair p; write_raw(p.a, raw_frame(10, 1, "ab")); p.a = Socket(-1);
        check(read_one(p.b, t, out) == FrameStatus::PEER_CLOSED_MIDFRAME, "FIN inside the body -> PEER_CLOSED_MIDFRAME");
    }
    {   // FIN after only 2 of the 4 header bytes
        Pair p; write_raw(p.a, std::string(2, '\0')); p.a = Socket(-1);
        check(read_one(p.b, t, out) == FrameStatus::PEER_CLOSED_MIDFRAME, "FIN inside the header -> PEER_CLOSED_MIDFRAME");
    }
}

static void test_bytes_read_count() {
    std::puts("Test 8: recv_all reports how many bytes arrived");
    Pair p; write_raw(p.a, "abc"); p.a = Socket(-1);
    char buf[8]; std::size_t got = 99;
    check(recv_all(p.b.fd(), buf, 8, &got) == IOStatus::PEER_CLOSED, "FIN before 8 bytes -> PEER_CLOSED");
    check(got == 3, "bytes_read == 3");
}

static void test_send_limits() {
    std::puts("Test 9: send_frame never emits a frame receivers would reject");
    Pair p; MsgType t; std::string out;
    check(send_frame(p.a.fd(), MsgType::CHAT, std::string(MAX_PAYLOAD + 1, 'x')) == FrameStatus::PROTOCOL_VIOLATION,
          "payload > MAX_PAYLOAD is refused");
    p.a = Socket(-1);
    check(read_one(p.b, t, out) == FrameStatus::PEER_CLOSED, "nothing was written to the socket");
}

static void test_large_transfer() {
    std::puts("Test 10: 1 MiB through tiny socket buffers with a slow reader");
    Pair p;
    int small = 1024;
    ::setsockopt(p.a.fd(), SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    ::setsockopt(p.b.fd(), SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));

    std::string data(1 << 20, '\0');
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(i * 31 + 7);

    std::thread w([&] { send_all(p.a.fd(), data.data(), data.size()); });
    std::string got(data.size(), '\0');
    bool ok = true;
    std::size_t off = 0;
    while (off < got.size() && ok) {
        const std::size_t chunk = std::min<std::size_t>(777, got.size() - off);   // odd size on purpose
        ok = recv_all(p.b.fd(), got.data() + off, chunk) == IOStatus::OK;
        off += chunk;
    }
    w.join();
    check(ok && got == data, "all 1048576 bytes arrive in order and unmodified");
}

// A no-op handler installed WITHOUT SA_RESTART: a signal then makes a blocked
// recv() fail with EINTR instead of being silently restarted by the kernel.
static void noop_handler(int) {}

static void test_eintr() {
    std::puts("Test 11: recv_all survives EINTR");
    struct sigaction sa {};
    sa.sa_handler = noop_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, nullptr);

    Pair p;
    IOStatus result = IOStatus::ERROR;
    char buf[5] = {};
    std::thread reader([&] { result = recv_all(p.b.fd(), buf, 5); });

    for (int i = 0; i < 5; ++i) {                    // interrupt the blocked recv() repeatedly
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        pthread_kill(reader.native_handle(), SIGUSR1);
    }
    write_raw(p.a, "hello");
    reader.join();
    check(result == IOStatus::OK && std::memcmp(buf, "hello", 5) == 0,
          "recv_all retried after 5 interruptions and returned the data");
}

int main() {
    std::puts("=== test_framing ===");
    test_round_trip();
    test_header_bytes();
    test_fragmentation();
    test_coalescing();
    test_invalid_lengths();
    test_unknown_type();
    test_closes();
    test_bytes_read_count();
    test_send_limits();
    test_large_transfer();
    test_eintr();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
