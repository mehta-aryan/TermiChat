// tests/test_framing.cpp – unit tests for the framing layer.
// Uses plain assert + a small check() helper.  No external test framework.
// Exercises: round-trip, big-endian header bytes, fragmentation, coalescing,
// invalid lengths, mid-frame close, max-payload with small SO_SNDBUF.

#include "../src/socket_utils.hpp"
#include "../src/protocol.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#include <sys/socket.h>   // socketpair, setsockopt
#include <arpa/inet.h>    // htonl
#include <unistd.h>       // write, close, read

// ── Helpers ──────────────────────────────────────────────────────────────────
static int g_pass = 0, g_fail = 0;

static void check(bool cond, const char* label) {
    if (cond) {
        std::printf("  PASS: %s\n", label);
        ++g_pass;
    } else {
        std::printf("  FAIL: %s\n", label);
        ++g_fail;
    }
}

// Returns a socket-pair {reader_fd, writer_fd}.
static std::pair<int,int> make_pair() {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::perror("socketpair");
        std::exit(1);
    }
    return {fds[0], fds[1]};
}

// ── Test 1: round-trip ───────────────────────────────────────────────────────
static void test_round_trip() {
    std::puts("Test 1: encode/decode round-trip");
    auto [rd, wr] = make_pair();

    std::string payload = "hello world";
    FrameStatus st = send_frame(wr, MsgType::CHAT, payload);
    check(st == FrameStatus::OK, "send_frame returns OK");

    MsgType     type;
    std::string out;
    st = recv_frame(rd, type, out);
    check(st == FrameStatus::OK,         "recv_frame returns OK");
    check(type == MsgType::CHAT,          "type is CHAT");
    check(out == payload,                 "payload matches");

    ::close(rd); ::close(wr);
}

// ── Test 2: big-endian header bytes ──────────────────────────────────────────
// A CHAT frame with payload "hi" (2 bytes) must produce exactly:
//   00 00 00 03  01  68 69
// length=3 (1 type byte + 2 payload bytes), type=1, 'h'=0x68, 'i'=0x69.
static void test_header_bytes() {
    std::puts("Test 2: header bytes are big-endian");
    auto [rd, wr] = make_pair();

    send_frame(wr, MsgType::CHAT, "hi");

    uint8_t buf[7];
    IOStatus st = recv_all(rd, buf, 7);
    check(st == IOStatus::OK, "recv_all gets 7 bytes");

    // Expected: 00 00 00 03 01 68 69
    check(buf[0] == 0x00 && buf[1] == 0x00 &&
          buf[2] == 0x00 && buf[3] == 0x03, "length field is big-endian 0x00000003");
    check(buf[4] == 0x01,  "type byte is CHAT=1");
    check(buf[5] == 0x68 && buf[6] == 0x69, "payload bytes are 'h','i'");

    ::close(rd); ::close(wr);
}

// ── Test 3: fragmentation ────────────────────────────────────────────────────
// A writer thread sends one frame one byte at a time with tiny delays.
// recv_frame must reassemble it correctly.
static void test_fragmentation() {
    std::puts("Test 3: fragmentation (one byte at a time with delays)");
    auto [rd, wr] = make_pair();

    std::string payload = "fragmented";

    // Build the raw frame bytes.
    uint32_t body_len = 1 + static_cast<uint32_t>(payload.size());
    uint32_t net_len  = htonl(body_len);
    std::string frame(4 + body_len, '\0');
    std::memcpy(frame.data(), &net_len, 4);
    frame[4] = static_cast<char>(MsgType::CHAT);
    std::memcpy(frame.data() + 5, payload.data(), payload.size());

    std::thread writer([wr, &frame]() {
        for (char c : frame) {
            ::send(wr, &c, 1, MSG_NOSIGNAL);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ::close(wr);
    });

    MsgType     type;
    std::string out;
    FrameStatus st = recv_frame(rd, type, out);
    check(st == FrameStatus::OK,     "recv_frame handles fragmented delivery");
    check(out == payload,             "payload correct after fragmentation");

    writer.join();
    ::close(rd);
}

// ── Test 4: coalescing ───────────────────────────────────────────────────────
// Two frames written in one send() are read back as two separate frames.
static void test_coalescing() {
    std::puts("Test 4: coalescing (two frames in one send)");
    auto [rd, wr] = make_pair();

    // Build two frames back-to-back in one buffer.
    auto make_frame = [](const std::string& p) -> std::string {
        uint32_t body_len = 1 + static_cast<uint32_t>(p.size());
        uint32_t net_len  = htonl(body_len);
        std::string f(4 + body_len, '\0');
        std::memcpy(f.data(), &net_len, 4);
        f[4] = static_cast<char>(MsgType::CHAT);
        std::memcpy(f.data() + 5, p.data(), p.size());
        return f;
    };
    std::string combined = make_frame("first") + make_frame("second");
    send_all(wr, combined.data(), combined.size());

    MsgType     type;
    std::string out1, out2;
    FrameStatus st1 = recv_frame(rd, type, out1);
    FrameStatus st2 = recv_frame(rd, type, out2);

    check(st1 == FrameStatus::OK && out1 == "first",  "first frame correct");
    check(st2 == FrameStatus::OK && out2 == "second", "second frame correct");

    ::close(rd); ::close(wr);
}

// ── Test 5: invalid lengths ──────────────────────────────────────────────────
// length=0 and length=0xFFFFFFFF must be rejected as protocol violations
// without performing a large allocation.
static void test_invalid_lengths() {
    std::puts("Test 5: invalid length rejection");

    // length = 0
    {
        auto [rd, wr] = make_pair();
        uint32_t bad = htonl(0);
        ::send(wr, &bad, 4, MSG_NOSIGNAL);
        ::close(wr);
        MsgType     t; std::string p;
        FrameStatus st = recv_frame(rd, t, p);
        check(st == FrameStatus::PROTOCOL_VIOLATION, "length=0 → PROTOCOL_VIOLATION");
        ::close(rd);
    }

    // length = 0xFFFFFFFF
    {
        auto [rd, wr] = make_pair();
        uint32_t bad = htonl(0xFFFFFFFF);
        ::send(wr, &bad, 4, MSG_NOSIGNAL);
        ::close(wr);
        MsgType     t; std::string p;
        FrameStatus st = recv_frame(rd, t, p);
        check(st == FrameStatus::PROTOCOL_VIOLATION, "length=0xFFFFFFFF → PROTOCOL_VIOLATION");
        ::close(rd);
    }
}

// ── Test 6: mid-frame close ──────────────────────────────────────────────────
// Close the connection after the length header but before all body bytes.
// recv_frame must return PEER_CLOSED_MIDFRAME (not PEER_CLOSED).
static void test_midframe_close() {
    std::puts("Test 6: mid-frame close detection");
    auto [rd, wr] = make_pair();

    // Write a valid header claiming 10 bytes, then close without sending body.
    uint32_t net_len = htonl(10);
    ::send(wr, &net_len, 4, MSG_NOSIGNAL);
    ::close(wr);  // close mid-frame

    MsgType     t; std::string p;
    FrameStatus st = recv_frame(rd, t, p);
    check(st == FrameStatus::PEER_CLOSED_MIDFRAME,
          "mid-frame close → PEER_CLOSED_MIDFRAME");
    ::close(rd);
}

// ── Test 7: max-payload with small SO_SNDBUF ─────────────────────────────────
// A maximum-size payload must pass through a socketpair with a deliberately
// small SO_SNDBUF, exercising the partial-send loop in send_all.
static void test_max_payload_small_buf() {
    std::puts("Test 7: max payload through small SO_SNDBUF");
    auto [rd, wr] = make_pair();

    // Shrink the kernel send buffer to force multiple send() calls.
    int small_buf = 256;
    ::setsockopt(wr, SOL_SOCKET, SO_SNDBUF, &small_buf, sizeof(small_buf));

    std::string payload(MAX_CHAT_TEXT, 'X');  // 1024 bytes

    // Send in a background thread (may block until reader drains the buffer).
    std::thread writer([wr, &payload]() {
        send_frame(wr, MsgType::CHAT, payload);
        ::close(wr);
    });

    MsgType     type;
    std::string out;
    FrameStatus st = recv_frame(rd, type, out);
    check(st == FrameStatus::OK,         "max-payload send_all loop works");
    check(out == payload,                 "max-payload content correct");

    writer.join();
    ::close(rd);
}

// ── main ─────────────────────────────────────────────────────────────────────
int main() {
    std::puts("=== test_framing ===");
    test_round_trip();
    test_header_bytes();
    test_fragmentation();
    test_coalescing();
    test_invalid_lengths();
    test_midframe_close();
    test_max_payload_small_buf();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
