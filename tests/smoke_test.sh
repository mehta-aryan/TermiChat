#!/usr/bin/env bash
# tests/smoke_test.sh – drives the real termichat_server / termichat_client.
#
# Usage:  tests/smoke_test.sh [runs]        (default: 1 run)
# Env:    SERVER_BIN, CLIENT_BIN            override the binary paths
#
# Every client reads stdin from its own FIFO.  The test keeps one write end of
# each FIFO open (fds 3-8); closing that fd is how we deliver EOF ("Ctrl+D").
# Raw protocol cases use bash's built-in /dev/tcp, not an external tool; the
# frames are written by hand as  | 4-byte length | type | payload |  with
# types JOIN=1 SAY=4 LIST_REQ=7 QUIT=9.

SERVER_BIN=${SERVER_BIN:-./termichat_server}
CLIENT_BIN=${CLIENT_BIN:-./termichat_client}
RUNS=${1:-1}

PASSES=0
FAILS=0
T=""            # per-run temp dir
SERVER_PID=""
PORT=""
declare -A CPID CFD

# ── tiny helpers ─────────────────────────────────────────────────────────────
pass() { echo "  PASS: $1"; PASSES=$((PASSES + 1)); }
fail() { echo "  FAIL: $1"; FAILS=$((FAILS + 1)); }
check() { # check <label> <command...>
    local label=$1; shift
    if "$@"; then pass "$label"; else fail "$label"; fi
}

alive() { # true if the process exists and is not a zombie
    [[ -r /proc/$1/stat ]] && ! grep -qE '^[0-9]+ \(.*\) Z ' "/proc/$1/stat" 2>/dev/null
}

# wait_for <file> <literal text> <timeout_s>: poll instead of sleeping blindly
wait_for() {
    local file=$1 text=$2 max=$(( $3 * 20 )) i=0
    while (( i < max )); do
        grep -aqF -- "$text" "$file" 2>/dev/null && return 0
        sleep 0.05; i=$((i + 1))
    done
    return 1
}

# wait_for_re <file> <extended regex> <timeout_s>
wait_for_re() {
    local file=$1 re=$2 max=$(( $3 * 20 )) i=0
    while (( i < max )); do
        grep -aqE -- "$re" "$file" 2>/dev/null && return 0
        sleep 0.05; i=$((i + 1))
    done
    return 1
}

# wait_for_count <file> <literal text> <n> <timeout_s>: at least n matching lines
wait_for_count() {
    local file=$1 text=$2 n=$3 max=$(( $4 * 20 )) i=0
    while (( i < max )); do
        (( $(grep -acF -- "$text" "$file" 2>/dev/null) >= n )) && return 0
        sleep 0.05; i=$((i + 1))
    done
    return 1
}

# wait_exit <pid> <timeout_s>: sets EXIT_CODE; returns 1 if still running
wait_exit() {
    local pid=$1 max=$(( $2 * 20 )) i=0
    while alive "$pid"; do
        (( i >= max )) && return 1
        sleep 0.05; i=$((i + 1))
    done
    { wait "$pid"; } 2>/dev/null
    EXIT_CODE=$?
    return 0
}

contains()     { grep -aqF -- "$2" "$T/out_$1"; }          # contains <client> <text>
not_contains() { ! grep -aqF -- "$2" "$T/out_$1"; }
count_of()     { grep -acF -- "$2" "$T/out_$1"; }          # count_of <client> <text>

# Close every inherited FIFO fd so a child never keeps another client's write
# end open (that would stop EOF from ever reaching that client).
CLOSE_FDS='exec 3>&- 4>&- 5>&- 6>&- 7>&- 8>&- 9>&-'

# launch_client <label> <fd> <username>: start a client and return at once
launch_client() {
    local label=$1 fd=$2 user=$3 fifo="$T/in_$1"
    mkfifo "$fifo"
    eval "exec $fd<>\"\$fifo\""            # parent keeps a write end open
    ( eval "$CLOSE_FDS"
      exec "$CLIENT_BIN" localhost "$PORT" "$user" < "$fifo" > "$T/out_$label" 2>&1 ) &
    CPID[$label]=$!
    CFD[$label]=$fd
}

# start_client <username> <fd>: launch, then wait until the server logged the join
start_client() {
    launch_client "$1" "$2" "$1"
    wait_for "$T/server.log" "$1 joined from" 5
}

say() { local name=$1; shift; printf '%s\n' "$*" >&"${CFD[$name]}"; }
close_stdin() { eval "exec ${CFD[$1]}>&-"; }

# Send raw bytes to the server through bash's /dev/tcp, hold the connection for
# a moment, then hang up.  $1 is a printf format (may contain \x escapes); $2
# (optional) is a count of 'A' bytes to append.
raw_send() {
    ( eval "$CLOSE_FDS"
      exec 9<>"/dev/tcp/127.0.0.1/$PORT" || exit 1
      printf "$1" >&9
      [[ -n "${2:-}" ]] && head -c "$2" /dev/zero | tr '\0' 'A' >&9
      sleep 0.3 )
}

# Send raw bytes and store everything the server answers (until it hangs up,
# at most 3 s) in $T/reply.
raw_reply() {
    ( eval "$CLOSE_FDS"
      exec 9<>"/dev/tcp/127.0.0.1/$PORT" || exit 1
      printf "$1" >&9
      timeout 3 cat <&9 > "$T/reply" )
}
reply_has() { grep -aqF -- "$1" "$T/reply"; }

start_server() {
    local attempt
    for attempt in 1 2 3 4 5 6 7 8 9 10; do
        PORT=$(( 20000 + (RANDOM * 32768 + RANDOM) % 40001 ))
        "$SERVER_BIN" "$PORT" > "$T/server.log" 2>&1 &
        SERVER_PID=$!
        local i=0
        while (( i < 60 )); do
            grep -aqF "Listening on" "$T/server.log" 2>/dev/null && return 0
            alive "$SERVER_PID" || break             # died: port was in use
            sleep 0.05; i=$((i + 1))
        done
        { wait "$SERVER_PID"; } 2>/dev/null
    done
    return 1
}

cleanup_run() {
    local p
    for p in "${CPID[@]}" "$SERVER_PID"; do
        [[ -n "$p" ]] && kill -9 "$p" 2>/dev/null
    done
    { wait; } 2>/dev/null
    eval "$CLOSE_FDS"
    [[ -n "$T" && -d "$T" ]] && rm -rf "$T"
    T=""; CPID=(); CFD=(); SERVER_PID=""
}
trap cleanup_run EXIT

# ── one full run ─────────────────────────────────────────────────────────────
run_once() {
    local before_fail=$FAILS
    T=$(mktemp -d)

    check "server starts and listens" start_server
    if [[ -z "$PORT" ]] || ! alive "$SERVER_PID"; then fail "cannot continue without a server"; cleanup_run; return; fi
    echo "  (port $PORT)"

    echo "── 1: alice, bob and carol join"
    check "alice joined" start_client alice 3
    check "bob joined" start_client bob 4
    check "carol joined" start_client carol 5
    check "server logged three raw connections" wait_for_count "$T/server.log" " connected" 3 3
    check "log line: 'carol joined from 127.0.0.1:port (3 online)'" \
        wait_for_re "$T/server.log" 'INFO\] carol joined from 127\.0\.0\.1:[0-9]+ \(3 online\)$' 3

    echo "── 2: welcome line and join notices"
    check "alice is welcomed (online: just her)" wait_for "$T/out_alice" "*** Welcome to TermiChat, alice! Online (1): alice" 3
    check "carol's welcome lists everyone, sorted" wait_for "$T/out_carol" "*** Welcome to TermiChat, carol! Online (3): alice, bob, carol" 3
    check "alice was told bob joined" wait_for "$T/out_alice" "*** bob joined the chat" 3
    check "alice was told carol joined" wait_for "$T/out_alice" "*** carol joined the chat" 3
    check "bob was told carol joined" wait_for "$T/out_bob" "*** carol joined the chat" 3
    check "nobody is told about their own join" bash -c \
        '! grep -aqF "alice joined the chat" "$1" && ! grep -aqF "bob joined the chat" "$2" && ! grep -aqF "carol joined the chat" "$3"' \
        _ "$T/out_alice" "$T/out_bob" "$T/out_carol"

    echo "── 3: a message reaches everyone, including the sender (echo)"
    local hm1 hm2
    hm1=$(date +%H:%M)
    say alice "hello from alice"
    check "bob got '[HH:MM] alice: hello from alice'" wait_for_re "$T/out_bob" '^\[[0-9]{2}:[0-9]{2}\] alice: hello from alice$' 3
    check "carol got it" wait_for_re "$T/out_carol" '^\[[0-9]{2}:[0-9]{2}\] alice: hello from alice$' 3
    check "alice got her own message back" wait_for_re "$T/out_alice" '^\[[0-9]{2}:[0-9]{2}\] alice: hello from alice$' 3
    hm2=$(date +%H:%M)
    check "the shown time is the current time" grep -aqE "^\[($hm1|$hm2)\] alice: hello from alice\$" "$T/out_bob"

    echo "── 4: text rules (separator, tab, trimming, controls, UTF-8)"
    say alice "a|b|c"
    check "'|' inside text is kept" wait_for_re "$T/out_bob" '\] alice: a\|b\|c$' 3
    say alice $'tab\there'
    check "tab becomes a space" wait_for_re "$T/out_bob" '\] alice: tab here$' 3
    say alice "   padded   "
    check "leading/trailing spaces are trimmed" wait_for_re "$T/out_bob" '\] alice: padded$' 3
    printf '\x1b\x07\n' >&"${CFD[alice]}"                 # nothing left after sanitising
    printf 'bin[\x00] caf\xc3\xa9 \xff end4\n' >&"${CFD[alice]}"
    check "NUL dropped, UTF-8 kept, bad byte -> '?'" wait_for "$T/out_bob" $'alice: bin[] caf\xc3\xa9 ? end4' 3
    check "a message that is empty after sanitising is not relayed" bash -c \
        '! grep -aqE "alice: *$" "$1"' _ "$T/out_bob"
    local big; big=$(head -c 1024 /dev/zero | tr '\0' x)
    say alice "$big"
    check "a 1024-byte message is delivered" wait_for_re "$T/out_bob" "\] alice: x{1024}\$" 3
    big=$(head -c 1025 /dev/zero | tr '\0' y)
    say alice "$big"
    check "a 1025-byte line is refused locally" wait_for "$T/out_alice" "*** Message too long (max 1024 bytes), not sent" 3
    say alice "marker-4"
    check "bob sees the marker" wait_for "$T/out_bob" "alice: marker-4" 3
    check "the 1025-byte line never reached bob" not_contains bob "yyyyyyyy"
    check "alice was not disconnected for it" alive "${CPID[alice]}"

    echo "── 5: /users, /help, unknown command; commands are never chat"
    say bob "/users"
    check "/users lists everyone" wait_for "$T/out_bob" "*** Online (3): alice, bob, carol" 3
    say bob "/help"
    check "/help is answered locally" wait_for "$T/out_bob" "*** Commands:" 3
    say bob "/nonsense arg"
    check "unknown command" wait_for "$T/out_bob" "*** Unknown command: /nonsense (try /help)" 3
    say bob "marker-5"
    check "alice sees marker-5" wait_for "$T/out_alice" "bob: marker-5" 3
    check "no command text was sent as chat" bash -c \
        '! grep -aqE "/users|/help|/nonsense" "$1"' _ "$T/out_alice"

    echo "── 6: same name in a different case is refused"
    launch_client dup 7 ALICE
    if wait_exit "${CPID[dup]}" 3; then
        check "ALICE exits with code 1 (got $EXIT_CODE)" test "$EXIT_CODE" -eq 1
    else
        fail "ALICE did not exit within 3 s"
    fi
    close_stdin dup
    check "the client says 'already taken'" contains dup "already taken"
    check "server logged '<ip:port> rejected: Username already taken'" \
        wait_for_re "$T/server.log" 'WARN\] 127\.0\.0\.1:[0-9]+ rejected: Username already taken$' 3
    check "alice is still the owner of the name" alive "${CPID[alice]}"

    echo "── 7: invalid and reserved names are refused by the server (raw JOIN frames)"
    raw_reply '\x00\x00\x00\x09\x01bad name'
    check "space in name -> reason sent" reply_has "may only contain"
    raw_reply '\x00\x00\x00\x07\x01SERVER'
    check "reserved name 'SERVER' -> reason sent" reply_has "is reserved"
    raw_reply '\x00\x00\x00\x07\x01System'
    check "reserved name 'System' -> reason sent" reply_has "is reserved"
    raw_reply '\x00\x00\x00\x12\x01abcdefghijklmnopq'
    check "17-character name -> reason sent" reply_has "1-16 characters"
    raw_reply '\x00\x00\x00\x01\x01'
    check "empty name -> reason sent" reply_has "1-16 characters"
    raw_reply '\x00\x00\x00\x04\x01a\x1bb'
    check "name with an ESC byte -> reason sent" reply_has "may only contain"
    check "server logged all 7 rejections (1 duplicate + 6 raw)" wait_for_count "$T/server.log" " rejected: " 7 3
    check "no ESC byte ever reached the server log" bash -c '! grep -aqF "$2" "$1"' _ "$T/server.log" $'\x1b'
    check "none of these was announced as a join" test "$(count_of alice "joined the chat")" -eq 2

    echo "── 8: two simultaneous JOINs of the same name: exactly one wins"
    launch_client r1 7 racer
    launch_client r2 8 Racer
    local i=0 won=0 lost=0
    while (( i < 100 )); do
        won=$(grep -alF "Welcome to TermiChat" "$T/out_r1" "$T/out_r2" 2>/dev/null | wc -l)
        lost=$(grep -alF "already taken" "$T/out_r1" "$T/out_r2" 2>/dev/null | wc -l)
        (( won == 1 && lost == 1 )) && break
        sleep 0.05; i=$((i + 1))
    done
    check "one client was welcomed (got $won)" test "$won" -eq 1
    check "the other was refused (got $lost)" test "$lost" -eq 1
    check "the server logged exactly one join of that name" \
        test "$(grep -aicF "racer joined from" "$T/server.log")" -eq 1
    local winner=r1 loser=r2
    grep -aqF "already taken" "$T/out_r1" && { winner=r2; loser=r1; }
    if wait_exit "${CPID[$loser]}" 3; then
        check "the loser exits with code 1 (got $EXIT_CODE)" test "$EXIT_CODE" -eq 1
    else
        fail "the loser did not exit"
    fi
    check "the winner is still connected" alive "${CPID[$winner]}"
    close_stdin "$loser"
    close_stdin "$winner"
    if wait_exit "${CPID[$winner]}" 3; then
        check "the winner leaves cleanly on EOF (code $EXIT_CODE)" test "$EXIT_CODE" -eq 0
    else
        fail "the winner did not exit on EOF"
    fi

    echo "── 9: /quit exits 0 and the others see the leave notice"
    say carol "/quit"
    if wait_exit "${CPID[carol]}" 3; then
        check "carol exited within 3 s with code 0 (got $EXIT_CODE)" test "$EXIT_CODE" -eq 0
    else
        fail "carol did not exit after /quit"
    fi
    check "carol printed no 'Server closed'" not_contains carol "Server closed"
    check "alice saw '*** carol left the chat'" wait_for "$T/out_alice" "*** carol left the chat" 3
    check "bob saw '*** carol left the chat'" wait_for "$T/out_bob" "*** carol left the chat" 3
    check "server logged '[INFO] carol left (quit) (n online)'" \
        wait_for_re "$T/server.log" 'INFO\] carol left \(quit\) \([0-9]+ online\)$' 3

    echo "── 10: stdin EOF (Ctrl+D) behaves like /quit"
    check "erin joined" start_client erin 7
    close_stdin erin
    if wait_exit "${CPID[erin]}" 3; then
        check "erin exited within 3 s with code 0 (got $EXIT_CODE)" test "$EXIT_CODE" -eq 0
    else
        fail "erin did not exit on stdin EOF"
    fi
    check "erin printed no 'Server closed'" not_contains erin "Server closed"
    check "alice saw '*** erin left the chat'" wait_for "$T/out_alice" "*** erin left the chat" 3
    check "server logged erin's quit" wait_for_re "$T/server.log" 'INFO\] erin left \(quit\)' 3

    echo "── 11: kill -9 of bob: the others get a 'lost connection' notice"
    kill -9 "${CPID[bob]}"; { wait "${CPID[bob]}"; } 2>/dev/null; close_stdin bob
    check "alice saw '*** bob lost connection'" wait_for "$T/out_alice" "*** bob lost connection" 3
    check "server logged a WARN for bob's departure" wait_for_re "$T/server.log" 'WARN\] bob left \(' 3
    check "server process still alive" alive "$SERVER_PID"
    check "dave joined after the kill" start_client dave 6
    check "dave's welcome shows only the survivors" wait_for "$T/out_dave" "*** Welcome to TermiChat, dave! Online (2): alice, dave" 3
    check "alice was told dave joined" wait_for "$T/out_alice" "*** dave joined the chat" 3
    say alice "hi dave"
    check "alice -> dave works" wait_for "$T/out_dave" "alice: hi dave" 3

    echo "── 12: protocol abuse by raw clients (server must survive, others unaffected)"
    local lost_before; lost_before=$(grep -acE 'left the chat|lost connection' "$T/out_alice")
    raw_send ''
    check "connect-and-hang-up before JOIN is logged" wait_for "$T/server.log" "rejected: connection closed before JOIN" 3
    raw_send '\x00\x00\x00\x03\x04hi'
    check "SAY before JOIN -> dropped" wait_for_count "$T/server.log" "rejected: expected JOIN as the first frame" 1 3
    raw_send '\x00\x00\x00\x01\x07'
    raw_send '\x00\x00\x00\x01\x09'
    check "LIST_REQ / QUIT before JOIN -> dropped" wait_for_count "$T/server.log" "rejected: expected JOIN as the first frame" 3 3
    raw_send '\x00\x00\x00\x02\x02x'
    check "a server-only type (JOIN_OK) as first frame -> dropped" wait_for_count "$T/server.log" "rejected: expected JOIN as the first frame" 4 3
    raw_send '\xff\xff\xff\xff'
    check "length 0xFFFFFFFF -> dropped" wait_for "$T/server.log" "rejected: protocol violation (bad length or unknown type)" 3
    raw_send '\x00\x00\x00\x02\x7fx'
    check "unknown type -> dropped" wait_for_count "$T/server.log" "protocol violation (bad length or unknown type)" 2 3
    raw_send '\x00\x00'
    check "header cut off after 2 bytes -> reported as truncated" wait_for "$T/server.log" "mid-frame (truncated frame)" 3
    raw_send '\x00\x00\x00\x06\x01twice\x00\x00\x00\x06\x01twice'
    check "second JOIN -> dropped" wait_for "$T/server.log" "twice left (protocol violation (JOIN sent twice))" 3
    raw_send '\x00\x00\x00\x09\x01bigmouth\x00\x00\x08\x01\x04' 2048
    check "2048-byte SAY after joining (valid frame, too long) -> dropped" \
        wait_for "$T/server.log" "bigmouth left (protocol violation (chat text longer than 1024 bytes))" 3
    raw_send '\x00\x00\x00\x08\x01mallory\x00\x00\x00\x0c\x04a\x1b[31mred\x07!'
    check "alice saw mallory join" wait_for "$T/out_alice" "*** mallory joined the chat" 3
    check "ESC and BEL are stripped for receivers" wait_for "$T/out_dave" "mallory: a[31mred!" 3
    check "no ESC byte reached dave" not_contains dave $'\x1b'
    check "no BEL byte reached dave" not_contains dave $'\x07'
    check "an abusive joined client is reported as 'lost connection'" wait_for "$T/out_alice" "*** bigmouth lost connection" 3
    check "alice saw twice lost connection" wait_for "$T/out_alice" "*** twice lost connection" 3
    check "alice saw mallory lost connection" wait_for "$T/out_alice" "*** mallory lost connection" 3
    check "server process still alive" alive "$SERVER_PID"
    say alice "still here"
    check "alice -> dave still works after the abuse" wait_for "$T/out_dave" "alice: still here" 3
    say dave "and me"
    check "alice received dave's message" wait_for "$T/out_alice" "dave: and me" 3
    check "the oversized text was never relayed" not_contains dave "AAAAAAAA"
    check "nobody got a 'Protocol error'" bash -c \
        '! grep -aqF "Protocol error" "$1" && ! grep -aqF "Protocol error" "$2"' _ "$T/out_alice" "$T/out_dave"
    check "a client that never joined produced no leave notice" \
        test "$(grep -acE 'left the chat|lost connection' "$T/out_alice")" -eq $(( lost_before + 3 ))
    # (+3: twice, bigmouth and mallory joined, then left; pre-JOIN violators gave no notice)

    echo "── 13: error paths"
    local dead=$(( PORT + 1 ))
    while (exec 9<>"/dev/tcp/127.0.0.1/$dead") 2>/dev/null; do dead=$((dead + 1)); done
    local out ec before after
    out=$(timeout 10 "$CLIENT_BIN" localhost "$dead" tester </dev/null 2>&1); ec=$?
    check "connection refused -> exit 1 (got $ec)" test "$ec" -eq 1
    check "refused message is clear" grep -qF "Connection refused" <<<"$out"
    out=$(timeout 15 "$CLIENT_BIN" nonexistent.invalid "$PORT" tester </dev/null 2>&1); ec=$?
    check "unknown host -> exit 1 (got $ec)" test "$ec" -eq 1
    check "unknown host message mentions getaddrinfo" grep -qF "getaddrinfo" <<<"$out"
    check "unknown host message has no bogus 'Success'" bash -c '! grep -qF "Success" <<<"$1"' _ "$out"
    out=$(timeout 10 "$SERVER_BIN" "$PORT" 2>&1); ec=$?
    check "second server on a used port -> exit 1 (got $ec)" test "$ec" -eq 1
    check "address-in-use message is clear" grep -qF "Address already in use" <<<"$out"
    timeout 5 "$CLIENT_BIN" localhost 99999 tester >/dev/null 2>&1 </dev/null; ec=$?
    check "client with bad port -> exit 2 (got $ec)" test "$ec" -eq 2
    timeout 5 "$SERVER_BIN" abc >/dev/null 2>&1; ec=$?
    check "server with bad port -> exit 2 (got $ec)" test "$ec" -eq 2
    out=$(timeout 5 "$CLIENT_BIN" localhost "$PORT" </dev/null 2>&1); ec=$?
    check "missing username without a tty -> exit 2 (got $ec)" test "$ec" -eq 2
    check "...and the usage text is printed" grep -qF "Usage:" <<<"$out"
    before=$(grep -acF " connected" "$T/server.log")
    out=$(timeout 5 "$CLIENT_BIN" localhost "$PORT" "bad name" </dev/null 2>&1); ec=$?
    check "invalid username -> exit 2 (got $ec)" test "$ec" -eq 2
    out=$(timeout 5 "$CLIENT_BIN" localhost "$PORT" server </dev/null 2>&1); ec=$?
    check "reserved username -> exit 2 (got $ec)" test "$ec" -eq 2
    check "...and it says why" grep -qF "reserved" <<<"$out"
    timeout 5 "$CLIENT_BIN" localhost "$PORT" tester extra </dev/null >/dev/null 2>&1; ec=$?
    check "too many arguments -> exit 2 (got $ec)" test "$ec" -eq 2
    after=$(grep -acF " connected" "$T/server.log")
    check "bad usernames were refused locally (no connection was made)" test "$before" -eq "$after"

    echo "── 14: server killed with SIGTERM while clients are connected"
    kill -TERM "$SERVER_PID"
    for c in alice dave; do
        if wait_exit "${CPID[$c]}" 3; then
            check "client $c exit code is 1 (got $EXIT_CODE)" test "$EXIT_CODE" -eq 1
        else
            fail "client $c did not exit within 3 s of the server dying"
        fi
        check "client $c printed 'Server closed'" contains "$c" "Server closed"
    done
    { wait "$SERVER_PID"; } 2>/dev/null

    cleanup_run
    if (( FAILS == before_fail )); then echo "  Run result: all checks passed"
    else echo "  Run result: $((FAILS - before_fail)) check(s) FAILED"; fi
}

for (( run = 1; run <= RUNS; run++ )); do
    echo
    echo "══════ Smoke test run $run / $RUNS ══════"
    run_once
done

echo
echo "══════ Total: $PASSES passed, $FAILS failed across $RUNS run(s) ══════"
(( FAILS == 0 ))
