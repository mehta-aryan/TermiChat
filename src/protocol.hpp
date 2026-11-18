#pragma once
// protocol.hpp – wire-protocol constants and frame send/recv helpers.
//
// Frame layout (TCP is a byte stream; without framing, message boundaries are
// lost – one send() does not equal one recv()):
//
//   | 4 bytes: length (big-endian, htonl) | 1 byte: type | payload (length-1 bytes) |
//
// 'length' counts the type byte plus the payload bytes.
// Phase 1 defines only CHAT = 1; the type byte makes it easy to add new
// message kinds in later phases without changing the framing.

#include "socket_utils.hpp"
#include <string>
#include <cstdint>

// ── Constants ────────────────────────────────────────────────────────────────
inline constexpr uint32_t MAX_PAYLOAD     = 2048;   // max (type + payload) bytes
inline constexpr uint32_t MAX_CHAT_TEXT   = 1024;   // max text the user may send

// ── Message types ─────────────────────────────────────────────────────────────
enum class MsgType : uint8_t {
    CHAT = 1,
};

// ── Frame I/O results ─────────────────────────────────────────────────────────
enum class FrameStatus {
    OK,
    PEER_CLOSED,          // clean TCP FIN arrived between frames
    PEER_CLOSED_MIDFRAME, // FIN arrived while reading the payload – truncated
    PROTOCOL_VIOLATION,   // bad length field or unknown type
    IO_ERROR,             // errno preserved from the failing recv/send
};

// send_frame: assembles header + type + payload into one buffer and writes it
// with a single send_all() call, so the frame can never be interleaved with
// another frame at the byte level.
FrameStatus send_frame(int fd, MsgType type, const std::string& payload);

// recv_frame: reads the 4-byte header, validates the length before allocating,
// then reads the body.  Distinguishes clean close from mid-frame truncation.
FrameStatus recv_frame(int fd, MsgType& out_type, std::string& out_payload);
