#pragma once
// protocol.hpp – wire-protocol constants, message codecs and frame send/recv.
//
// Frame layout (TCP is a byte stream; without framing, message boundaries are
// lost – one send() does not equal one recv()):
//
//   | 4 bytes: length (big-endian, htonl) | 1 byte: type | payload (length-1 bytes) |
//
// 'length' counts the type byte plus the payload bytes.
//
// Protocol v2 message types (C = client, S = server):
//
//   1 JOIN      C->S  username
//   2 JOIN_OK   S->C  canonical username
//   3 JOIN_ERR  S->C  human-readable reason (server closes right after)
//   4 SAY       C->S  chat text
//   5 MSG       S->C  <unix_ts>|<sender>|<text>
//   6 SYSTEM    S->C  <unix_ts>|<kind>|<text>   kind: info join leave error shutdown
//   7 LIST_REQ  C->S  (empty)
//   8 LIST_RES  S->C  name1,name2,...           sorted case-insensitively
//   9 QUIT      C->S  (empty)
//  10 PRIV_SAY  C->S  <target>|<text>                   one recipient, by name
//  11 PRIV_MSG  S->C  <unix_ts>|<from>|<to>|<text>      sent to recipient AND sender
//
// Structured payloads are split on the FIRST separators only, so the last
// field (the text) may itself contain '|'.  Usernames can never contain it.

#include "socket_utils.hpp"
#include <string>
#include <cstdint>
#include <vector>

// ── Constants ────────────────────────────────────────────────────────────────
inline constexpr uint32_t MAX_PAYLOAD     = 2048;   // max payload bytes (type byte not counted)
inline constexpr uint32_t MAX_CHAT_TEXT   = 1024;   // max text the user may send

// ── Message types ─────────────────────────────────────────────────────────────
enum class MsgType : uint8_t {
    JOIN     = 1,
    JOIN_OK  = 2,
    JOIN_ERR = 3,
    SAY      = 4,
    MSG      = 5,
    SYSTEM   = 6,
    LIST_REQ = 7,
    LIST_RES = 8,
    QUIT     = 9,
    PRIV_SAY = 10,
    PRIV_MSG = 11,
};

// True if 'raw' is one of the eleven types above.
bool is_known_type(uint8_t raw);

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
// another frame at the byte level.  Refuses payloads larger than MAX_PAYLOAD
// (returns PROTOCOL_VIOLATION without writing anything).
FrameStatus send_frame(int fd, MsgType type, const std::string& payload);

// recv_frame: reads the 4-byte header, validates the length before allocating,
// then reads the body.  Distinguishes clean close from mid-frame truncation.
FrameStatus recv_frame(int fd, MsgType& out_type, std::string& out_payload);

// ── Structured payloads ──────────────────────────────────────────────────────
enum class SystemKind { Info, Join, Leave, Error, Shutdown };

// Wire name of a kind ("info", "join", ...).
const char* system_kind_name(SystemKind kind);

// MSG: <unix_ts>|<sender>|<text>.  decode_* return false on a malformed
// payload (missing separator, non-numeric or empty timestamp, empty sender,
// unknown kind) and leave the outputs unspecified.
std::string encode_msg(int64_t timestamp, const std::string& sender, const std::string& text);
bool decode_msg(const std::string& payload, int64_t& timestamp,
                std::string& sender, std::string& text);

// SYSTEM: <unix_ts>|<kind>|<text>
std::string encode_system(int64_t timestamp, SystemKind kind, const std::string& text);
bool decode_system(const std::string& payload, int64_t& timestamp,
                   SystemKind& kind, std::string& text);

// PRIV_SAY: <target>|<text>.  Only the format is checked here: a non-empty
// target and one separator.  The text may be empty (the server answers that
// with an error notice) and the target's validity is the server's business.
std::string encode_priv_say(const std::string& target, const std::string& text);
bool decode_priv_say(const std::string& payload, std::string& target, std::string& text);

// PRIV_MSG: <unix_ts>|<from>|<to>|<text>.  Same rules as MSG, plus a non-empty
// recipient name.
std::string encode_priv_msg(int64_t timestamp, const std::string& from,
                            const std::string& to, const std::string& text);
bool decode_priv_msg(const std::string& payload, int64_t& timestamp, std::string& from,
                     std::string& to, std::string& text);

// LIST_RES: name1,name2,...  An empty payload is an empty list; an empty
// element ("a,,b", ",a", "a,") is malformed.
std::string encode_user_list(const std::vector<std::string>& names);
bool decode_user_list(const std::string& payload, std::vector<std::string>& names);

// ── ChatEvent ────────────────────────────────────────────────────────────────
// What the client builds from each frame it receives; the UI only ever sees
// this struct, never raw payloads.
enum class EventKind { Message, System, UserList, Private };

struct ChatEvent {
    EventKind                kind        = EventKind::System;
    int64_t                  timestamp   = 0;                  // Message, System, Private
    std::string              sender;                           // Message, Private (from)
    std::string              recipient;                        // Private (to)
    std::string              text;                             // Message, System, Private
    SystemKind               system_kind = SystemKind::Info;   // System
    std::vector<std::string> users;                            // UserList
};

// Builds a ChatEvent from a received MSG, PRIV_MSG, SYSTEM or LIST_RES frame.  Returns
// false for any other type or a malformed payload (a protocol error).
bool make_event(MsgType type, const std::string& payload, ChatEvent& out);
