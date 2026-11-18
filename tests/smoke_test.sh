#!/usr/bin/env bash
# tests/smoke_test.sh – multi-client integration test using the real binaries.
#
# Usage: bash tests/smoke_test.sh [run_count]
#   SERVER_BIN / CLIENT_BIN env vars override default binary paths.
#   Run from the project root.
#
# Client stdin is controlled via regular files + 'tail -f'.
# Sending a message: append a line to the client's input file.
# Simulating Ctrl+D (EOF): kill the tail process feeding the client's stdin.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
SERVER_BIN="${SERVER_BIN:-$PROJECT_ROOT/termichat_server}"
CLIENT_BIN="${CLIENT_BIN:-$PROJECT_ROOT/termichat_client}"
RUN_COUNT="${1:-3}"

pass_count=0
fail_count=0

# wait_for <file> <regex> <timeout_secs>
wait_for() {
    local file="$1" regex="$2" timeout_s="$3"
    local steps=$(( timeout_s * 10 ))
    local i=0
    while (( i < steps )); do
        if [[ -f "$file" ]] && grep -qE "$regex" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
        (( i++ ))
    done
    return 1
}

# pick_port: random port 20000-60000
pick_port() {
    local port attempts=0
    while (( attempts < 10 )); do
        port=$(( RANDOM % 40001 + 20000 ))
        if ! ss -tlnH "sport = :$port" 2>/dev/null | grep -q .; then
            echo "$port"; return 0
        fi
        (( attempts++ ))
    done
    echo "Could not find free port" >&2; exit 1
}

TMPDIR=""
SERVER_PID=""

cleanup() {
    [[ -n "$SERVER_PID" ]] && kill -9 "$SERVER_PID" 2>/dev/null || true
    jobs -p | xargs -r kill -9 2>/dev/null || true
    [[ -n "$TMPDIR" ]] && rm -rf "$TMPDIR"
}

run_one() {
    local run_num="$1"
    local run_pass=0 run_fail=0

    _pass() { echo "  PASS: $*"; (( run_pass++ )); }
    _fail() { echo "  FAIL: $*"; (( run_fail++ )); }

    echo ""
    echo "══════════════════════════════════════"
    echo " Smoke test run $run_num / $RUN_COUNT"
    echo "══════════════════════════════════════"

    TMPDIR="$(mktemp -d)"
    SERVER_PID=""
    trap cleanup EXIT INT TERM

    # ── Start server ────────────────────────────────────────────────────────
    local port
    local attempts=0
    while (( attempts < 10 )); do
        port=$(pick_port)
        "$SERVER_BIN" "$port" > "$TMPDIR/server.log" 2>&1 &
        SERVER_PID=$!
        if wait_for "$TMPDIR/server.log" "Listening on" 20; then
            break
        fi
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
        (( attempts++ ))
    done
    if [[ -z "$SERVER_PID" ]]; then
        _fail "Could not start server"
        return
    fi
    _pass "Server started on port $port"

    # ── Client management ────────────────────────────────────────────────────
    # Each client's stdin comes from: tail -f <input_file> | client
    # To send a line:   echo msg >> <input_file>
    # To signal EOF:    kill the tail process
    declare -A TAIL_PID=()
    declare -A CLIENT_PID=()

    start_client() {
        local name="$1"
        # Create the input file before tail opens it.
        touch "$TMPDIR/in_$name.txt"
        # tail -f follows the file; pipe its output to the client's stdin.
        # The client's stdout+stderr go to an output log file.
        tail -f "$TMPDIR/in_$name.txt" \
            | "$CLIENT_BIN" localhost "$port" \
              > "$TMPDIR/out_$name.log" 2>&1 &
        # In a pipeline, the last background job's PID is $!.
        # We also need the tail PID. Use $BASHPID inside the subshell trick:
        # Instead, use a temp file to capture tail's PID.
        TAIL_PID[$name]=$(jobs -p | tail -1)  # last background job = tail
        CLIENT_PID[$name]=$!
        # Give the pipeline a moment so we can get the right PIDs.
        sleep 0.05
        # Re-capture tail PID more reliably: it's the second-to-last job started.
        # Actually let's just track PIDs from /proc.
        # Simplest: capture tail PID by starting it separately.
        true
    }

    # More reliable: start tail and client in a way we can capture both PIDs.
    start_client2() {
        local name="$1"
        touch "$TMPDIR/in_$name.txt"
        # Start tail in background, capture its PID.
        tail -f "$TMPDIR/in_$name.txt" > "$TMPDIR/tail_$name.fifo" &
        TAIL_PID[$name]=$!
        # Actually piping is simpler. Use mkfifo for the tail→client pipe.
        mkfifo "$TMPDIR/pipe_$name"
        tail -f "$TMPDIR/in_$name.txt" > "$TMPDIR/pipe_$name" &
        TAIL_PID[$name]=$!
        "$CLIENT_BIN" localhost "$port" \
            < "$TMPDIR/pipe_$name" \
            > "$TMPDIR/out_$name.log" 2>&1 &
        CLIENT_PID[$name]=$!
    }

    send_msg() {
        local name="$1" msg="$2"
        echo "$msg" >> "$TMPDIR/in_$name.txt"
    }

    close_stdin() {
        local name="$1"
        # Killing tail closes the pipe write end; client sees EOF.
        kill "${TAIL_PID[$name]}" 2>/dev/null
        wait "${TAIL_PID[$name]}" 2>/dev/null || true
    }

    # ── Scenario 1 ──────────────────────────────────────────────────────────
    echo ""
    echo "── Scenario 1: Connect 3 clients ──"

    start_client2 A
    wait_for "$TMPDIR/server.log" "Client 1 connected" 20 \
        && _pass "Client A connected (id 1)" \
        || _fail "Client A did not connect"

    start_client2 B
    wait_for "$TMPDIR/server.log" "Client 2 connected" 20 \
        && _pass "Client B connected (id 2)" \
        || _fail "Client B did not connect"

    start_client2 C
    wait_for "$TMPDIR/server.log" "Client 3 connected" 20 \
        && _pass "Client C connected (id 3)" \
        || _fail "Client C did not connect"

    # ── Scenario 2 ──────────────────────────────────────────────────────────
    echo ""
    echo "── Scenario 2: Broadcast from A ──"
    sleep 0.2
    send_msg A "hello from A"
    sleep 0.3

    wait_for "$TMPDIR/out_B.log" '\[Client 1\].*hello from A' 10 \
        && _pass "B received A's message" \
        || _fail "B did NOT receive A's message"

    wait_for "$TMPDIR/out_C.log" '\[Client 1\].*hello from A' 10 \
        && _pass "C received A's message" \
        || _fail "C did NOT receive A's message"

    if grep -qE '\[Client 1\].*hello from A' "$TMPDIR/out_A.log" 2>/dev/null; then
        _fail "A received its own message (should not)"
    else
        _pass "A did not receive its own message"
    fi

    # ── Scenario 3 ──────────────────────────────────────────────────────────
    echo ""
    echo "── Scenario 3: Broadcast from B ──"
    send_msg B "greetings from B"
    sleep 0.3

    wait_for "$TMPDIR/out_A.log" '\[Client 2\].*greetings from B' 10 \
        && _pass "A received B's message" \
        || _fail "A did NOT receive B's message"

    wait_for "$TMPDIR/out_C.log" '\[Client 2\].*greetings from B' 10 \
        && _pass "C received B's message" \
        || _fail "C did NOT receive B's message"

    # ── Scenario 4 ──────────────────────────────────────────────────────────
    echo ""
    echo "── Scenario 4: kill -9 Client C ──"
    kill -9 "${CLIENT_PID[C]}" 2>/dev/null
    kill -9 "${TAIL_PID[C]}" 2>/dev/null
    wait "${CLIENT_PID[C]}" 2>/dev/null || true
    wait "${TAIL_PID[C]}"   2>/dev/null || true

    wait_for "$TMPDIR/server.log" "Client 3 (disconnected|I/O error)" 30 \
        && _pass "Server logged C's disconnect" \
        || _fail "Server did not log C's disconnect"

    send_msg A "after C killed"
    sleep 0.3
    wait_for "$TMPDIR/out_B.log" '\[Client 1\].*after C killed' 10 \
        && _pass "A->B still works after C killed" \
        || _fail "A->B broke after C killed"

    # ── Scenario 5 ──────────────────────────────────────────────────────────
    echo ""
    echo "── Scenario 5: New client D (id 4) ──"
    start_client2 D
    wait_for "$TMPDIR/server.log" "Client 4 connected" 20 \
        && _pass "Client D connected (id 4)" \
        || _fail "Client D did not connect"

    send_msg D "hello from D"
    sleep 0.3

    wait_for "$TMPDIR/out_A.log" '\[Client 4\].*hello from D' 10 \
        && _pass "A received D's message" \
        || _fail "A did NOT receive D's message"

    wait_for "$TMPDIR/out_B.log" '\[Client 4\].*hello from D' 10 \
        && _pass "B received D's message" \
        || _fail "B did NOT receive D's message"

    # ── Scenario 6 ──────────────────────────────────────────────────────────
    echo ""
    echo "── Scenario 6: Close A's stdin (Ctrl+D) ──"
    close_stdin A

    local deadline=20
    local elapsed=0
    local a_exited=false
    while (( elapsed < deadline )); do
        if ! kill -0 "${CLIENT_PID[A]}" 2>/dev/null; then
            a_exited=true; break
        fi
        sleep 0.1; (( elapsed++ ))
    done

    if $a_exited; then
        wait "${CLIENT_PID[A]}"; local ec=$?
        (( ec == 0 )) \
            && _pass "Client A exited 0 after stdin close" \
            || _fail "Client A exited $ec (expected 0)"
    else
        kill "${CLIENT_PID[A]}" 2>/dev/null; wait "${CLIENT_PID[A]}" 2>/dev/null || true
        _fail "Client A did not exit within 2 s of stdin close"
    fi

    wait_for "$TMPDIR/server.log" "Client 1 disconnected" 20 \
        && _pass "Server logged A's disconnect" \
        || _fail "Server did not log A's disconnect"

    # ── Scenario 7 ──────────────────────────────────────────────────────────
    echo ""
    echo "── Scenario 7: NUL + non-ASCII bytes ──"
    printf 'caf\xc3\xa9\x00byte\n' >> "$TMPDIR/in_B.txt"
    sleep 0.4
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        _pass "Server alive after NUL/non-ASCII message"
    else
        _fail "Server crashed on NUL/non-ASCII message"
    fi

    # ── Scenario 8 ──────────────────────────────────────────────────────────
    echo ""
    echo "── Scenario 8: Bad port / port in use ──"
    "$CLIENT_BIN" localhost 1 > /dev/null 2>&1; local ec_bad=$?
    (( ec_bad != 0 )) \
        && _pass "Client to bad port exits non-zero (ec=$ec_bad)" \
        || _fail "Client to bad port exited 0 (should be non-zero)"

    "$SERVER_BIN" "$port" > /dev/null 2>&1; local ec2=$?
    (( ec2 != 0 )) \
        && _pass "Second server on used port exits non-zero (ec=$ec2)" \
        || _fail "Second server on used port exited 0 (should be non-zero)"

    # ── Scenario 9 ──────────────────────────────────────────────────────────
    echo ""
    echo "── Scenario 9: kill -TERM server ──"
    kill -TERM "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""

    local b_done=false d_done=false
    local countdown=30
    while (( countdown > 0 )); do
        ! kill -0 "${CLIENT_PID[B]}" 2>/dev/null && b_done=true
        ! kill -0 "${CLIENT_PID[D]}" 2>/dev/null && d_done=true
        $b_done && $d_done && break
        sleep 0.1; (( countdown-- ))
    done

    if $b_done; then
        wait "${CLIENT_PID[B]}"; local eb=$?
        (( eb == 1 )) \
            && _pass "Client B exited 1 after server TERM" \
            || _fail "Client B exited $eb (expected 1)"
    else
        kill "${CLIENT_PID[B]}" 2>/dev/null; wait "${CLIENT_PID[B]}" 2>/dev/null || true
        _fail "Client B did not exit within 3 s"
    fi

    if $d_done; then
        wait "${CLIENT_PID[D]}"; local ed=$?
        (( ed == 1 )) \
            && _pass "Client D exited 1 after server TERM" \
            || _fail "Client D exited $ed (expected 1)"
    else
        kill "${CLIENT_PID[D]}" 2>/dev/null; wait "${CLIENT_PID[D]}" 2>/dev/null || true
        _fail "Client D did not exit within 3 s"
    fi

    wait_for "$TMPDIR/out_B.log" "Server closed" 10 \
        && _pass "Client B printed 'Server closed'" \
        || _fail "Client B did not print 'Server closed'"

    wait_for "$TMPDIR/out_D.log" "Server closed" 10 \
        && _pass "Client D printed 'Server closed'" \
        || _fail "Client D did not print 'Server closed'"

    echo ""
    echo "Run $run_num result: $run_pass passed, $run_fail failed"
    (( pass_count += run_pass ))
    (( fail_count += run_fail ))

    trap - EXIT INT TERM
    cleanup
    TMPDIR=""; SERVER_PID=""
}

for i in $(seq 1 "$RUN_COUNT"); do
    run_one "$i"
done

echo ""
echo "══════════════════════════════════════"
echo " Total: $pass_count passed, $fail_count failed across $RUN_COUNT runs"
echo "══════════════════════════════════════"

[[ $fail_count -eq 0 ]]
