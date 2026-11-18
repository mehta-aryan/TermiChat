// protocol.cpp – implementation of send_frame and recv_frame.

#include "protocol.hpp"

#include <arpa/inet.h>   // htonl, ntohl
#include <cstring>       // memcpy

FrameStatus send_frame(int fd, MsgType type, const std::string& payload) {
    // Never emit a frame that every receiver would reject as a protocol
    // violation (recv_frame enforces the same limit).
    if (payload.size() > MAX_PAYLOAD) return FrameStatus::PROTOCOL_VIOLATION;

    // Build the whole frame in one buffer so that a single send_all() call
    // writes it atomically from the kernel's perspective.  Two separate
    // send_all() calls could be interleaved with another thread's frame on
    // the same socket (the per-session send_mutex in ClientSession prevents
    // that too, but defence in depth is good).
    const uint32_t body_len = 1 + static_cast<uint32_t>(payload.size());
    const uint32_t net_len  = htonl(body_len); // big-endian on the wire

    // Frame: 4-byte length | 1-byte type | payload bytes
    std::string frame(4 + body_len, '\0');
    std::memcpy(frame.data(),     &net_len, 4);
    frame[4] = static_cast<char>(type);
    if (!payload.empty())
        std::memcpy(frame.data() + 5, payload.data(), payload.size());

    return send_all(fd, frame.data(), frame.size()) == IOStatus::OK
               ? FrameStatus::OK
               : FrameStatus::IO_ERROR;
}

FrameStatus recv_frame(int fd, MsgType& out_type, std::string& out_payload) {
    // Step 1: read exactly 4 bytes for the length field.
    // A FIN before any byte arrives is a normal close between frames.  A FIN
    // after 1-3 header bytes means the peer vanished mid-frame.
    uint32_t net_len = 0;
    std::size_t header_got = 0;
    IOStatus st = recv_all(fd, &net_len, 4, &header_got);
    if (st == IOStatus::PEER_CLOSED)
        return header_got == 0 ? FrameStatus::PEER_CLOSED
                               : FrameStatus::PEER_CLOSED_MIDFRAME;
    if (st == IOStatus::ERROR)       return FrameStatus::IO_ERROR;

    // Step 2: convert from big-endian and validate BEFORE allocating.
    // length 0 is invalid (there is always at least the type byte).
    // length > 1 + MAX_PAYLOAD would need a huge allocation – reject it.
    uint32_t body_len = ntohl(net_len);
    if (body_len == 0 || body_len > 1 + MAX_PAYLOAD)
        return FrameStatus::PROTOCOL_VIOLATION;

    // Step 3: read the body (type byte + payload).
    std::string body(body_len, '\0');
    st = recv_all(fd, body.data(), body_len);
    if (st == IOStatus::PEER_CLOSED) return FrameStatus::PEER_CLOSED_MIDFRAME;
    if (st == IOStatus::ERROR)       return FrameStatus::IO_ERROR;

    // Step 4: check the type byte.
    uint8_t raw_type = static_cast<uint8_t>(body[0]);
    if (raw_type != static_cast<uint8_t>(MsgType::CHAT))
        return FrameStatus::PROTOCOL_VIOLATION;

    out_type    = static_cast<MsgType>(raw_type);
    // Treat payload bytes as untrusted binary data – use (ptr, len), never
    // rely on a NUL terminator from the network.
    out_payload = std::string(body.data() + 1, body_len - 1);
    return FrameStatus::OK;
}
