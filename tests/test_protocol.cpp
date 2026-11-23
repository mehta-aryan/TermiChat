// tests/test_protocol.cpp – unit tests for the payload codecs (incl. private
// messages) and ChatEvent.
// No sockets involved: these are pure string functions.

#include "check.hpp"
#include "../src/protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

static void test_msg_codec() {
    std::puts("Test 1: MSG  <unix_ts>|<sender>|<text>");
    check(encode_msg(1700000000, "alice", "hello") == "1700000000|alice|hello",
          "encode_msg builds exactly 1700000000|alice|hello");

    int64_t ts = 0; std::string sender, text;
    check(decode_msg("1700000000|alice|hello", ts, sender, text) &&
          ts == 1700000000 && sender == "alice" && text == "hello", "plain message decodes");

    check(decode_msg("5|bob|a|b||c|", ts, sender, text) && sender == "bob" && text == "a|b||c|",
          "text containing '|' keeps every '|' (split on the first 2 only)");
    check(decode_msg("5|bob||", ts, sender, text) && sender == "bob" && text == "|",
          "text that is just '|' survives");
    check(decode_msg("5|bob|", ts, sender, text) && sender == "bob" && text.empty(),
          "empty text field is allowed");
    check(decode_msg(encode_msg(0, "x", "t"), ts, sender, text) && ts == 0, "timestamp 0 round-trips");

    check(!decode_msg("", ts, sender, text), "empty payload rejected");
    check(!decode_msg("123", ts, sender, text), "no separator rejected");
    check(!decode_msg("123|alice", ts, sender, text), "only one separator rejected");
    check(!decode_msg("|alice|hi", ts, sender, text), "empty timestamp rejected");
    check(!decode_msg("12x|alice|hi", ts, sender, text), "non-numeric timestamp rejected");
    check(!decode_msg("-5|alice|hi", ts, sender, text), "negative timestamp rejected");
    check(!decode_msg(" 5|alice|hi", ts, sender, text), "timestamp with a space rejected");
    check(!decode_msg("1234567890123456789|alice|hi", ts, sender, text), "19-digit timestamp rejected (overflow guard)");
    check(decode_msg("123456789012345678|alice|hi", ts, sender, text) && ts == 123456789012345678LL,
          "18-digit timestamp accepted");
    check(!decode_msg("5||hi", ts, sender, text), "empty sender rejected");
}

static void test_priv_say_codec() {
    std::puts("Test 1b: PRIV_SAY  <target>|<text>");
    check(encode_priv_say("bob", "hi") == "bob|hi", "encode_priv_say builds exactly bob|hi");

    std::string target, text;
    check(decode_priv_say("bob|hello there", target, text) && target == "bob" && text == "hello there",
          "plain private message decodes");
    check(decode_priv_say("bob|a|b||c|", target, text) && target == "bob" && text == "a|b||c|",
          "text containing '|' keeps every '|' (split on the FIRST one only)");
    check(decode_priv_say("bob||", target, text) && target == "bob" && text == "|",
          "text that is just '|' survives");
    check(decode_priv_say("bob|", target, text) && target == "bob" && text.empty(),
          "empty text decodes (the server answers it with an error notice)");

    const std::string uni = "caf\xc3\xa9 \xe2\x98\x83 \xf0\x9f\x98\x80";
    check(decode_priv_say(encode_priv_say("amy_1", uni), target, text) && target == "amy_1" && text == uni,
          "UTF-8 text round-trips byte for byte");

    check(!decode_priv_say("", target, text), "empty payload rejected");
    check(!decode_priv_say("bob", target, text), "no separator rejected");
    check(!decode_priv_say("|hi", target, text), "empty target rejected");
    check(!decode_priv_say("|", target, text), "lone separator rejected");
}

static void test_priv_msg_codec() {
    std::puts("Test 1c: PRIV_MSG  <unix_ts>|<from>|<to>|<text>");
    check(encode_priv_msg(1700000000, "alice", "bob", "psst") == "1700000000|alice|bob|psst",
          "encode_priv_msg builds exactly 1700000000|alice|bob|psst");

    int64_t ts = 0; std::string from, to, text;
    check(decode_priv_msg("1700000000|alice|bob|psst", ts, from, to, text) &&
          ts == 1700000000 && from == "alice" && to == "bob" && text == "psst", "plain message decodes");
    check(decode_priv_msg("5|a|b|x|y||z|", ts, from, to, text) && from == "a" && to == "b" && text == "x|y||z|",
          "text containing '|' keeps every '|' (split on the first 3 only)");
    check(decode_priv_msg("5|a|b|", ts, from, to, text) && text.empty(), "empty text field is allowed");

    const std::string uni = "\xe2\x98\x83 caf\xc3\xa9";
    check(decode_priv_msg(encode_priv_msg(7, "a", "b", uni), ts, from, to, text) && text == uni,
          "UTF-8 text round-trips byte for byte");

    check(!decode_priv_msg("", ts, from, to, text), "empty payload rejected");
    check(!decode_priv_msg("5|a|b", ts, from, to, text), "only two separators rejected");
    check(!decode_priv_msg("5|a", ts, from, to, text), "only one separator rejected");
    check(!decode_priv_msg("|a|b|hi", ts, from, to, text), "empty timestamp rejected");
    check(!decode_priv_msg("x5|a|b|hi", ts, from, to, text), "non-numeric timestamp rejected");
    check(!decode_priv_msg("-5|a|b|hi", ts, from, to, text), "negative timestamp rejected");
    check(!decode_priv_msg("1234567890123456789|a|b|hi", ts, from, to, text), "19-digit timestamp rejected");
    check(!decode_priv_msg("5||b|hi", ts, from, to, text), "empty sender rejected");
    check(!decode_priv_msg("5|a||hi", ts, from, to, text), "empty recipient rejected");
}

static void test_system_codec() {
    std::puts("Test 2: SYSTEM  <unix_ts>|<kind>|<text>");
    check(encode_system(1700000000, SystemKind::Join, "bob joined the chat")
              == "1700000000|join|bob joined the chat", "encode_system builds 1700000000|join|...");

    const SystemKind kinds[] = {SystemKind::Info, SystemKind::Join, SystemKind::Leave,
                                SystemKind::Error, SystemKind::Shutdown};
    const char* names[] = {"info", "join", "leave", "error", "shutdown"};
    bool all_ok = true;
    for (int i = 0; i < 5; ++i) {
        int64_t ts = 0; SystemKind k = SystemKind::Info; std::string text;
        const std::string wire = encode_system(42, kinds[i], "a|b");
        all_ok = all_ok && std::string(system_kind_name(kinds[i])) == names[i]
                        && wire == std::string("42|") + names[i] + "|a|b"
                        && decode_system(wire, ts, k, text) && ts == 42 && k == kinds[i] && text == "a|b";
    }
    check(all_ok, "all five kinds: wire names, round trip, '|' inside the text");

    int64_t ts = 0; SystemKind k = SystemKind::Info; std::string text = "unchanged";
    check(decode_system("9|info|", ts, k, text) && text.empty(), "empty text allowed");
    check(!decode_system("9|bogus|x", ts, k, text), "unknown kind rejected");
    check(!decode_system("9|INFO|x", ts, k, text), "kind names are case-sensitive");
    check(!decode_system("9||x", ts, k, text), "empty kind rejected");
    check(!decode_system("|info|x", ts, k, text), "empty timestamp rejected");
    check(!decode_system("x|info|x", ts, k, text), "non-numeric timestamp rejected");
    check(!decode_system("9|info", ts, k, text), "missing second separator rejected");
    check(!decode_system("", ts, k, text), "empty payload rejected");
}

static void test_user_list_codec() {
    std::puts("Test 3: LIST_RES  name1,name2,...");
    check(encode_user_list({"alice", "bob", "carol"}) == "alice,bob,carol", "encode joins with commas");
    check(encode_user_list({"solo"}) == "solo", "single name has no comma");

    std::vector<std::string> names;
    check(decode_user_list("alice,bob,carol", names) &&
          names == std::vector<std::string>({"alice", "bob", "carol"}), "three names decode in order");
    check(decode_user_list("solo", names) && names == std::vector<std::string>({"solo"}), "one name decodes");
    check(decode_user_list("", names) && names.empty(), "empty payload is an empty list");
    check(!decode_user_list("a,,b", names), "empty element in the middle rejected");
    check(!decode_user_list(",a", names), "leading comma rejected");
    check(!decode_user_list("a,", names), "trailing comma rejected");
    check(!decode_user_list(",", names), "lone comma rejected");
}

static void test_make_event() {
    std::puts("Test 4: make_event builds a ChatEvent from a received frame");
    ChatEvent ev;

    check(make_event(MsgType::MSG, "1700000000|alice|hi|there", ev) &&
          ev.kind == EventKind::Message && ev.timestamp == 1700000000 &&
          ev.sender == "alice" && ev.text == "hi|there", "MSG -> Message event");

    check(make_event(MsgType::SYSTEM, "7|leave|bob left the chat", ev) &&
          ev.kind == EventKind::System && ev.timestamp == 7 &&
          ev.system_kind == SystemKind::Leave && ev.text == "bob left the chat", "SYSTEM -> System event");

    check(make_event(MsgType::LIST_RES, "a,b", ev) &&
          ev.kind == EventKind::UserList && ev.users == std::vector<std::string>({"a", "b"}),
          "LIST_RES -> UserList event");

    const MsgType not_for_clients[] = {MsgType::JOIN, MsgType::JOIN_OK, MsgType::JOIN_ERR,
                                       MsgType::SAY, MsgType::PRIV_SAY, MsgType::LIST_REQ, MsgType::QUIT};
    bool rejected = true;
    for (MsgType t : not_for_clients) rejected = rejected && !make_event(t, "1|info|x", ev);
    check(rejected, "types that never travel to a client are rejected");

    check(make_event(MsgType::PRIV_MSG, "1700000000|alice|bob|a|b", ev) &&
          ev.kind == EventKind::Private && ev.timestamp == 1700000000 && ev.sender == "alice" &&
          ev.recipient == "bob" && ev.text == "a|b", "PRIV_MSG -> Private event with from/to");

    check(!make_event(MsgType::MSG, "garbage", ev), "malformed MSG rejected");
    check(!make_event(MsgType::PRIV_MSG, "5|alice|bob", ev), "malformed PRIV_MSG rejected");
    check(!make_event(MsgType::SYSTEM, "1|nope|x", ev), "malformed SYSTEM rejected");
    check(!make_event(MsgType::LIST_RES, "a,,b", ev), "malformed LIST_RES rejected");
}

static void test_size_invariant() {
    std::puts("Test 5: a worst-case relayed MSG never exceeds MAX_PAYLOAD");
    const std::string payload = encode_msg(9999999999LL, std::string(16, 'n'), std::string(MAX_CHAT_TEXT, 'x'));
    check(payload.size() <= MAX_PAYLOAD, "1024-byte text + 16-char name + timestamp fits one frame");

    const std::string priv = encode_priv_msg(9999999999LL, std::string(16, 'n'), std::string(16, 'm'),
                                             std::string(MAX_CHAT_TEXT, 'x'));
    check(priv.size() <= MAX_PAYLOAD, "worst-case PRIV_MSG (two 16-char names) fits one frame");
    check(encode_priv_say(std::string(16, 'n'), std::string(MAX_CHAT_TEXT, 'x')).size() <= MAX_PAYLOAD,
          "worst-case PRIV_SAY fits one frame");
}

static void test_type_numbers() {
    std::puts("Test 6: message type numbers are the protocol table");
    check(static_cast<int>(MsgType::JOIN) == 1 && static_cast<int>(MsgType::JOIN_OK) == 2 &&
          static_cast<int>(MsgType::JOIN_ERR) == 3 && static_cast<int>(MsgType::SAY) == 4 &&
          static_cast<int>(MsgType::MSG) == 5 && static_cast<int>(MsgType::SYSTEM) == 6 &&
          static_cast<int>(MsgType::LIST_REQ) == 7 && static_cast<int>(MsgType::LIST_RES) == 8 &&
          static_cast<int>(MsgType::QUIT) == 9, "JOIN=1 ... QUIT=9");
    check(static_cast<int>(MsgType::PRIV_SAY) == 10 && static_cast<int>(MsgType::PRIV_MSG) == 11,
          "PRIV_SAY=10, PRIV_MSG=11");
    check(!is_known_type(0) && is_known_type(1) && is_known_type(9) && is_known_type(10) &&
          is_known_type(11) && !is_known_type(12) && !is_known_type(255),
          "is_known_type accepts exactly 1..11");
}

int main() {
    std::puts("=== test_protocol ===");
    test_type_numbers();
    test_msg_codec();
    test_priv_say_codec();
    test_priv_msg_codec();
    test_system_codec();
    test_user_list_codec();
    test_make_event();
    test_size_invariant();
    return summary();
}
