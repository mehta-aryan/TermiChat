// protocol.cpp – frame send/recv and the structured-payload codecs.

#include "protocol.hpp"
#include "text_utils.hpp"

#include <arpa/inet.h>   // htonl, ntohl
#include <cstring>       // memcpy
#include <iterator>      // std::size

bool is_known_type(uint8_t raw) {
    return raw >= static_cast<uint8_t>(MsgType::JOIN) &&
           raw <= static_cast<uint8_t>(MsgType::PRIV_MSG);
}

FrameStatus send_frame(int fd, MsgType type, const std::string& payload, int send_flags) {
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

    return send_all(fd, frame.data(), frame.size(), send_flags) == IOStatus::OK
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
    if (!is_known_type(raw_type))
        return FrameStatus::PROTOCOL_VIOLATION;

    out_type    = static_cast<MsgType>(raw_type);
    // Treat payload bytes as untrusted binary data – use (ptr, len), never
    // rely on a NUL terminator from the network.
    out_payload = std::string(body.data() + 1, body_len - 1);
    return FrameStatus::OK;
}

// ── Structured payloads ──────────────────────────────────────────────────────
static const char* const SYSTEM_KIND_NAMES[] = {"info", "join", "leave", "error", "shutdown"};

const char* system_kind_name(SystemKind kind) {
    return SYSTEM_KIND_NAMES[static_cast<std::size_t>(kind)];
}

static bool parse_system_kind(const std::string& name, SystemKind& kind) {
    for (std::size_t i = 0; i < std::size(SYSTEM_KIND_NAMES); ++i) {
        if (name == SYSTEM_KIND_NAMES[i]) {
            kind = static_cast<SystemKind>(i);
            return true;
        }
    }
    return false;
}

// Splits 's' into exactly 'count' fields, cutting at the first count-1
// separators only; the last field keeps any further separators.
static bool split_fields(const std::string& s, char sep, std::size_t count,
                         std::vector<std::string>& fields) {
    fields.clear();
    std::size_t start = 0;
    while (fields.size() + 1 < count) {
        const std::size_t pos = s.find(sep, start);
        if (pos == std::string::npos) return false;
        fields.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    fields.push_back(s.substr(start));
    return true;
}

// Digits only, 1-18 of them (so the value always fits in int64_t).
static bool parse_timestamp(const std::string& s, int64_t& out) {
    if (s.empty() || s.size() > 18) return false;
    int64_t value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

std::string encode_msg(int64_t timestamp, const std::string& sender, const std::string& text) {
    return std::to_string(timestamp) + '|' + sender + '|' + text;
}

bool decode_msg(const std::string& payload, int64_t& timestamp,
                std::string& sender, std::string& text) {
    std::vector<std::string> f;
    if (!split_fields(payload, '|', 3, f)) return false;
    if (!parse_timestamp(f[0], timestamp)) return false;
    if (f[1].empty()) return false;
    sender = std::move(f[1]);
    text   = std::move(f[2]);
    return true;
}

std::string encode_priv_say(const std::string& target, const std::string& text) {
    return target + '|' + text;
}

bool decode_priv_say(const std::string& payload, std::string& target, std::string& text) {
    std::vector<std::string> f;
    if (!split_fields(payload, '|', 2, f)) return false;
    if (f[0].empty()) return false;
    target = std::move(f[0]);
    text   = std::move(f[1]);
    return true;
}

std::string encode_priv_msg(int64_t timestamp, const std::string& from,
                            const std::string& to, const std::string& text) {
    return std::to_string(timestamp) + '|' + from + '|' + to + '|' + text;
}

bool decode_priv_msg(const std::string& payload, int64_t& timestamp, std::string& from,
                     std::string& to, std::string& text) {
    std::vector<std::string> f;
    if (!split_fields(payload, '|', 4, f)) return false;
    if (!parse_timestamp(f[0], timestamp)) return false;
    if (f[1].empty() || f[2].empty()) return false;
    from = std::move(f[1]);
    to   = std::move(f[2]);
    text = std::move(f[3]);
    return true;
}

std::string encode_system(int64_t timestamp, SystemKind kind, const std::string& text) {
    return std::to_string(timestamp) + '|' + system_kind_name(kind) + '|' + text;
}

bool decode_system(const std::string& payload, int64_t& timestamp,
                   SystemKind& kind, std::string& text) {
    std::vector<std::string> f;
    if (!split_fields(payload, '|', 3, f)) return false;
    if (!parse_timestamp(f[0], timestamp)) return false;
    if (!parse_system_kind(f[1], kind)) return false;
    text = std::move(f[2]);
    return true;
}

std::string encode_user_list(const std::vector<std::string>& names) {
    return join_strings(names, ",");
}

bool decode_user_list(const std::string& payload, std::vector<std::string>& names) {
    names.clear();
    if (payload.empty()) return true;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = payload.find(',', start);
        const std::size_t end = (pos == std::string::npos) ? payload.size() : pos;
        if (end == start) return false;                  // empty element
        names.push_back(payload.substr(start, end - start));
        if (pos == std::string::npos) return true;
        start = pos + 1;
    }
}

bool make_event(MsgType type, const std::string& payload, ChatEvent& out) {
    ChatEvent ev;
    switch (type) {
    case MsgType::MSG:
        ev.kind = EventKind::Message;
        if (!decode_msg(payload, ev.timestamp, ev.sender, ev.text)) return false;
        break;
    case MsgType::PRIV_MSG:
        ev.kind = EventKind::Private;
        if (!decode_priv_msg(payload, ev.timestamp, ev.sender, ev.recipient, ev.text)) return false;
        break;
    case MsgType::SYSTEM:
        ev.kind = EventKind::System;
        if (!decode_system(payload, ev.timestamp, ev.system_kind, ev.text)) return false;
        break;
    case MsgType::LIST_RES:
        ev.kind = EventKind::UserList;
        if (!decode_user_list(payload, ev.users)) return false;
        break;
    default:
        return false;
    }
    out = std::move(ev);
    return true;
}
