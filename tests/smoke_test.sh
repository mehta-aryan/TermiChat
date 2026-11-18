#!/usr/bin/env bash
# tests/smoke_test.sh – drives the real termichat_server / termichat_client.
#
# Usage:  tests/smoke_test.sh [runs]        (default: 1 run)
# Env:    SERVER_BIN, CLIENT_BIN            override the binary paths
#
# Every client reads stdin from its own FIFO.  The test keeps one write end of
# each FIFO open (fds 3-6); closing that fd is how we deliver EOF ("Ctrl+D").
# Raw protocol-abuse cases use bash's built-in /dev/tcp, not an external tool.

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

# Close every inherited FIFO fd so a child never keeps another client's write
# end open (that would stop EOF from ever reaching that client).
CLOSE_FDS='exec 3>&- 4>&- 5>&- 6>&- 7>&- 8>&- 9>&-'

start_client() { # start_client <name> <fd> <expected id>
    local name=$1 fd=$2 id=$3 fifo="$T/in_$1"
    mkfifo "$fifo"
    eval "exec $fd<>\"\$fifo\""            # parent keeps a write end open
    ( eval "$CLOSE_FDS"
      exec "$CLIENT_BIN" localhost "$PORT" < "$fifo" > "$T/out_$name" 2>&1 ) &
    CPID[$name]=$!
    CFD[$name]=$fd
    wait_for "$T/server.log" "Client $id connected" 5
}

say() { local name=$1; shift; printf '%s\n' "$*" >&"${CFD[$name]}"; }
close_stdin() { eval "exec ${CFD[$1]}>&-"; }

# Send raw bytes to the server through bash's /dev/tcp, then hang up.
# $1 is a printf format (may contain \x escapes); $2 (optional) is a count of
# 'A' bytes to append.
raw_send() {
    ( eval "$CLOSE_FDS"
      exec 9<>"/dev/tcp/127.0.0.1/$PORT" || exit 1
      printf "$1" >&9
      [[ -n "${2:-}" ]] && head -c "$2" /dev/zero | tr '\0' 'A' >&9
      sleep 0.3 )
}

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

    echo "── 1: three clients connect (ids 1, 2, 3 in accept order)"
    check "client A connected as id 1" start_client A 3 1
    check "client B connected as id 2" start_client B 4 2
    check "client C connected as id 3" start_client C 5 3

    echo "── 2: broadcast from A reaches B and C, not A"
    say A "hello from A"
    check "B received A's message" wait_for "$T/out_B" "[Client 1]: hello from A" 3
    check "C received A's message" wait_for "$T/out_C" "[Client 1]: hello from A" 3

    echo "── 3: broadcast from B"
    say B "hello from B"
    check "A received B's message" wait_for "$T/out_A" "[Client 2]: hello from B" 3
    check "C received B's message" wait_for "$T/out_C" "[Client 2]: hello from B" 3
    # TCP keeps order per connection: any echo of A's own message would have
    # reached A before B's message did, so this check is deterministic.
    check "A never received its own message (no echo)" not_contains A "hello from A"

    echo "── 4: kill -9 client C; server survives"
    kill -9 "${CPID[C]}"; { wait "${CPID[C]}"; } 2>/dev/null; close_stdin C
    check "server logged C's disconnect" wait_for "$T/server.log" "Client 3 disconnected" 3
    check "server process still alive" alive "$SERVER_PID"
    say A "after the kill"
    check "A -> B still works" wait_for "$T/out_B" "[Client 1]: after the kill" 3

    echo "── 5: a new client D joins (id 4)"
    check "client D connected as id 4" start_client D 6 4
    say D "hi from D"
    check "A received D's message" wait_for "$T/out_A" "[Client 4]: hi from D" 3
    check "B received D's message" wait_for "$T/out_B" "[Client 4]: hi from D" 3
    say A "welcome D"
    check "D received A's message" wait_for "$T/out_D" "[Client 1]: welcome D" 3

    echo "── 6: stdin EOF (Ctrl+D) on A: exit code 0, quietly"
    close_stdin A
    if wait_exit "${CPID[A]}" 2; then
        check "A exited within 2 s" true
        check "A's exit code is 0 (got $EXIT_CODE)" test "$EXIT_CODE" -eq 0
    else
        fail "A did not exit within 2 s of stdin close"
    fi
    check "A did not print 'Server closed' when the user quit" not_contains A "Server closed"
    check "server logged A's disconnect" wait_for "$T/server.log" "Client 1 disconnected (peer closed connection)" 3

    echo "── 7: NUL and non-ASCII bytes in a message"
    printf 'bin[\x00] caf\xc3\xa9 \xff end7\n' >&"${CFD[B]}"
    check "D received the binary message" wait_for "$T/out_D" "end7" 3
    check "server process still alive" alive "$SERVER_PID"

    echo "── 8: protocol abuse by raw clients (server must survive, others unaffected)"
    raw_send '\xff\xff\xff\xff'
    check "length 0xFFFFFFFF -> dropped" wait_for "$T/server.log" "protocol violation (bad length or unknown type)" 3
    raw_send '\x00\x00\x00\x02\x7fx'
    check "unknown type -> dropped" wait_for_count "$T/server.log" "protocol violation (bad length or unknown type)" 2 3
    raw_send '\x00\x00\x08\x01\x01' 2048
    check "2048-byte chat text (valid frame, too long) -> dropped" wait_for "$T/server.log" "chat text longer than 1024 bytes" 3
    raw_send '\x00\x00'
    check "header cut off after 2 bytes -> reported as truncated" wait_for "$T/server.log" "mid-frame (truncated frame)" 3
    check "server process still alive" alive "$SERVER_PID"
    say B "still here"
    check "B -> D still works after the abuse" wait_for "$T/out_D" "[Client 2]: still here" 3
    check "B was not kicked by an oversized relay" not_contains B "Protocol error"
    check "D was not kicked by an oversized relay" not_contains D "Protocol error"

    echo "── 9: error paths"
    local dead=$(( PORT + 1 ))
    while (exec 9<>"/dev/tcp/127.0.0.1/$dead") 2>/dev/null; do dead=$((dead + 1)); done
    local out ec
    out=$(timeout 10 "$CLIENT_BIN" localhost "$dead" 2>&1); ec=$?
    check "connection refused -> exit 1 (got $ec)" test "$ec" -eq 1
    check "refused message is clear" grep -qF "Connection refused" <<<"$out"
    out=$(timeout 15 "$CLIENT_BIN" nonexistent.invalid "$PORT" 2>&1); ec=$?
    check "unknown host -> exit 1 (got $ec)" test "$ec" -eq 1
    check "unknown host message mentions getaddrinfo" grep -qF "getaddrinfo" <<<"$out"
    check "unknown host message has no bogus 'Success'" bash -c '! grep -qF "Success" <<<"$1"' _ "$out"
    out=$(timeout 10 "$SERVER_BIN" "$PORT" 2>&1); ec=$?
    check "second server on a used port -> exit 1 (got $ec)" test "$ec" -eq 1
    check "address-in-use message is clear" grep -qF "Address already in use" <<<"$out"
    timeout 5 "$CLIENT_BIN" localhost 99999 >/dev/null 2>&1; ec=$?
    check "client with bad port -> exit 2 (got $ec)" test "$ec" -eq 2
    timeout 5 "$SERVER_BIN" abc >/dev/null 2>&1; ec=$?
    check "server with bad port -> exit 2 (got $ec)" test "$ec" -eq 2

    echo "── 10: server killed with SIGTERM while clients are connected"
    kill -TERM "$SERVER_PID"
    for c in B D; do
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
