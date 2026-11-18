#!/usr/bin/env bash
# tests/smoke_test.sh – multi-client integration test using real binaries.
#
# Usage: bash tests/smoke_test.sh [run_count]
#   SERVER_BIN / CLIENT_BIN env vars may override default binary paths.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
SERVER_BIN="${SERVER_BIN:-$PROJECT_ROOT/termichat_server}"
CLIENT_BIN="${CLIENT_BIN:-$PROJECT_ROOT/termichat_client}"
RUN_COUNT="${1:-3}"

pass_count=0
fail_count=0

# wait_for <file> <regex> <timeout_tenths>
# Polls file for regex every 0.1 s.
wait_for() {
    local file="$1" regex="$2" timeout_tenths="$3"
    local i=0
    while (( i < timeout_tenths )); do
        if [[ -f "$file" ]] && grep -qE "$regex" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
        (( i++ ))
    done
    return 1
}

# pick_port: random port in 20000-60000 (no external tools used)
pick_port() {
    echo $(( RANDOM % 40001 + 20000 ))
}

TMPDIR=""
SERVER_PID=""

cleanup() {
    # Close any open test client file descriptors
    exec 3>&- 4>&- 5>&- 6>&- 7>&- 8>&- 9>&- 2>/dev/null || true

    if [[ -n "${SERVER_PID:-}" ]]; then
        kill -9 "$SERVER_PID" 2>/dev/null || true
    fi
    jobs -p | xargs -r kill -9 2>/dev/null || true
    if [[ -n "${TMPDIR:-}" && -d "$TMPDIR" ]]; then
        rm -rf "$TMPDIR"
    fi
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

    # ── Start server (retry up to 10 random ports if port in use) ───────────
    local port
    local attempts=0
    while (( attempts < 10 )); do
        port=$(pick_port)
        "$SERVER_BIN" "$port" > "$TMPDIR/server.log" 2>&1 &
        SERVER_PID=$!
        # Wait up to 2 seconds for server to bind and listen
        if wait_for "$TMPDIR/server.log" "Listening on 0.0.0.0:$port" 20; then
            break
        fi
        kill -9 "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
        (( attempts++ ))
    done

    if [[ -z "$SERVER_PID" ]]; then
        _fail "Server failed to start after 10 attempts"
        return 1
    fi
    _pass "Server listening on port $port"

    # ── FIFO-based client management ────────────────────────────────────────
    # Assign distinct file descriptors for writing to client FIFOs:
    # A=3, B=4, C=5, D=6
    declare -A CLIENT_FD=([A]=3 [B]=4 [C]=5 [D]=6)
    declare -A CLIENT_PID=()

    start_client() {
        local name="$1"
        local fd="${CLIENT_FD[$name]}"
        mkfifo "$TMPDIR/fifo_$name"

        # Launch client reading stdin from the FIFO.
        # CRITICAL: Close all client write FDs (3..9) in the child process
        # so child processes do not inherit the write end of any FIFO!
        "$CLIENT_BIN" localhost "$port" \
            < "$TMPDIR/fifo_$name" \
            > "$TMPDIR/out_$name.log" 2>&1 3>&- 4>&- 5>&- 6>&- 7>&- 8>&- 9>&- &
        CLIENT_PID[$name]=$!

        # Open the write end in the parent test shell.
        eval "exec $fd> \"$TMPDIR/fifo_$name\""
    }

    send_msg() {
        local name="$1"
        local msg="$2"
        local fd="${CLIENT_FD[$name]}"
        echo "$msg" >&"$fd"
    }

    close_stdin() {
        local name="$1"
        local fd="${CLIENT_FD[$name]}"
        eval "exec $fd>&-"
    }

    # ── Scenario 1: Start clients A, B, C one at a time ─────────────────────
    echo ""
    echo "── Scenario 1: Connect clients A, B, C ──"
    start_client A
    wait_for "$TMPDIR/server.log" "Client 1 connected" 20 \
        && _pass "Client A connected (assigned ID 1)" \
        || _fail "Client A failed to connect"

    start_client B
    wait_for "$TMPDIR/server.log" "Client 2 connected" 20 \
        && _pass "Client B connected (assigned ID 2)" \
        || _fail "Client B failed to connect"

    start_client C
    wait_for "$TMPDIR/server.log" "Client 3 connected" 20 \
        && _pass "Client C connected (assigned ID 3)" \
        || _fail "Client C failed to connect"

    # ── Scenario 2: A sends; B and C receive; A does not self-echo ───────────
    echo ""
    echo "── Scenario 2: Broadcast from Client A ──"
    sleep 0.1
    send_msg A "hello from A"

    wait_for "$TMPDIR/out_B.log" '\[Client 1\]: hello from A' 15 \
        && _pass "Client B received '[Client 1]: hello from A'" \
        || _fail "Client B did not receive message from A"

    wait_for "$TMPDIR/out_C.log" '\[Client 1\]: hello from A' 15 \
        && _pass "Client C received '[Client 1]: hello from A'" \
        || _fail "Client C did not receive message from A"

    if grep -qE '\[Client 1\]: hello from A' "$TMPDIR/out_A.log" 2>/dev/null; then
        _fail "Client A received its own message (unexpected self-echo)"
    else
        _pass "Client A did not receive its own message (no self-echo)"
    fi

    # ── Scenario 3: B sends; A and C receive ─────────────────────────────────
    echo ""
    echo "── Scenario 3: Broadcast from Client B ──"
    send_msg B "greetings from B"

    wait_for "$TMPDIR/out_A.log" '\[Client 2\]: greetings from B' 15 \
        && _pass "Client A received '[Client 2]: greetings from B'" \
        || _fail "Client A did not receive message from B"

    wait_for "$TMPDIR/out_C.log" '\[Client 2\]: greetings from B' 15 \
        && _pass "Client C received '[Client 2]: greetings from B'" \
        || _fail "Client C did not receive message from B"

    # ── Scenario 4: kill -9 C; server survives; A->B still works ─────────────
    echo ""
    echo "── Scenario 4: Abrupt kill -9 of Client C ──"
    kill -9 "${CLIENT_PID[C]}" 2>/dev/null || true
    wait "${CLIENT_PID[C]}" 2>/dev/null || true

    wait_for "$TMPDIR/server.log" "Client 3 (disconnected|I/O error)" 30 \
        && _pass "Server logged Client 3 disconnect within 3 s" \
        || _fail "Server did not log Client 3 disconnect within 3 s"

    send_msg A "after C killed"
    wait_for "$TMPDIR/out_B.log" '\[Client 1\]: after C killed' 15 \
        && _pass "A -> B broadcast works after C was killed" \
        || _fail "Broadcast failed after C was killed"

    # ── Scenario 5: Client D connects (ID 4) ─────────────────────────────────
    echo ""
    echo "── Scenario 5: New Client D connects (ID 4) ──"
    start_client D
    wait_for "$TMPDIR/server.log" "Client 4 connected" 20 \
        && _pass "Client D connected (assigned ID 4)" \
        || _fail "Client D failed to connect"

    send_msg D "hello from D"
    wait_for "$TMPDIR/out_A.log" '\[Client 4\]: hello from D' 15 \
        && _pass "Client A received message from D" \
        || _fail "Client A did not receive message from D"

    wait_for "$TMPDIR/out_B.log" '\[Client 4\]: hello from D' 15 \
        && _pass "Client B received message from D" \
        || _fail "Client B did not receive message from D"

    # ── Scenario 6: Close A's stdin (Ctrl+D); exit 0 within 2 s ─────────────
    echo ""
    echo "── Scenario 6: Close Client A stdin (Ctrl+D simulation) ──"
    close_stdin A

    local elapsed=0
    local a_exited=false
    while (( elapsed < 20 )); do
        if ! kill -0 "${CLIENT_PID[A]}" 2>/dev/null; then
            a_exited=true
            break
        fi
        sleep 0.1
        (( elapsed++ ))
    done

    if $a_exited; then
        wait "${CLIENT_PID[A]}" 2>/dev/null; local ec=$?
        if (( ec == 0 )); then
            _pass "Client A exited with code 0 within 2 s"
        else
            _fail "Client A exited with code $ec (expected 0)"
        fi
    else
        kill -9 "${CLIENT_PID[A]}" 2>/dev/null || true
        _fail "Client A did not exit within 2 s of stdin close"
    fi

    wait_for "$TMPDIR/server.log" "Client 1 disconnected" 20 \
        && _pass "Server logged Client 1 disconnect" \
        || _fail "Server did not log Client 1 disconnect"

    # ── Scenario 7: NUL byte + non-ASCII bytes ────────────────────────────────
    echo ""
    echo "── Scenario 7: Message with NUL and non-ASCII bytes ──"
    printf 'caf\xc3\xa9\x00extra\n' >&"${CLIENT_FD[B]}"
    sleep 0.4
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        _pass "Server survived NUL and non-ASCII binary payload"
    else
        _fail "Server crashed on NUL and non-ASCII payload"
    fi

    # ── Scenario 8: Bad port / port already in use ───────────────────────────
    echo ""
    echo "── Scenario 8: Error handling (bad port / port in use) ──"
    # Port 1 (privileged / unusable or refused)
    "$CLIENT_BIN" localhost 1 > "$TMPDIR/bad_client.log" 2>&1 || local ec_bad=$?
    if (( ec_bad != 0 )) && grep -qiE "connect|error" "$TMPDIR/bad_client.log" 2>/dev/null; then
        _pass "Client to closed port exited non-zero with error message"
    else
        _fail "Client to closed port did not exit properly"
    fi

    # Second server attempting to bind same port
    "$SERVER_BIN" "$port" > "$TMPDIR/dup_server.log" 2>&1 || local ec_dup=$?
    if (( ec_dup != 0 )) && grep -qiE "bind|address already in use" "$TMPDIR/dup_server.log" 2>/dev/null; then
        _pass "Second server on used port exited non-zero with bind error message"
    else
        _fail "Second server on used port did not fail as expected"
    fi

    # ── Scenario 9: kill -TERM server; remaining clients exit 1 within 3 s ───
    echo ""
    echo "── Scenario 9: kill -TERM server ──"
    kill -TERM "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""

    local b_exited=false d_exited=false
    local countdown=30
    while (( countdown > 0 )); do
        ! kill -0 "${CLIENT_PID[B]}" 2>/dev/null && b_exited=true
        ! kill -0 "${CLIENT_PID[D]}" 2>/dev/null && d_exited=true
        $b_exited && $d_exited && break
        sleep 0.1
        (( countdown-- ))
    done

    if $b_exited; then
        wait "${CLIENT_PID[B]}" 2>/dev/null; local ec_b=$?
        if (( ec_b == 1 )); then
            _pass "Client B exited with code 1 within 3 s"
        else
            _fail "Client B exited with code $ec_b (expected 1)"
        fi
    else
        kill -9 "${CLIENT_PID[B]}" 2>/dev/null || true
        _fail "Client B did not exit within 3 s"
    fi

    if $d_exited; then
        wait "${CLIENT_PID[D]}" 2>/dev/null; local ec_d=$?
        if (( ec_d == 1 )); then
            _pass "Client D exited with code 1 within 3 s"
        else
            _fail "Client D exited with code $ec_d (expected 1)"
        fi
    else
        kill -9 "${CLIENT_PID[D]}" 2>/dev/null || true
        _fail "Client D did not exit within 3 s"
    fi

    wait_for "$TMPDIR/out_B.log" "Server closed" 10 \
        && _pass "Client B printed 'Server closed' notice" \
        || _fail "Client B did not print 'Server closed'"

    wait_for "$TMPDIR/out_D.log" "Server closed" 10 \
        && _pass "Client D printed 'Server closed' notice" \
        || _fail "Client D did not print 'Server closed'"

    # ── Summary for this run ─────────────────────────────────────────────────
    echo ""
    echo "Run $run_num result: $run_pass passed, $run_fail failed"
    (( pass_count += run_pass ))
    (( fail_count += run_fail ))

    trap - EXIT INT TERM
    cleanup
    TMPDIR=""
    SERVER_PID=""

    [[ $run_fail -eq 0 ]]
}

for i in $(seq 1 "$RUN_COUNT"); do
    if ! run_one "$i"; then
        echo "Run $i failed!"
        exit 1
    fi
done

echo ""
echo "══════════════════════════════════════"
echo " Total: $pass_count passed, $fail_count failed across $RUN_COUNT runs"
echo "══════════════════════════════════════"

[[ $fail_count -eq 0 ]]
