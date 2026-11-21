#!/bin/sh

# nixsk integration test suite
# POSIX shell
# FreeBSD / Linux compatible

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(dirname "$SCRIPT_DIR")

TEST_NAME="nixsk integration tests"

TMP_BASE=${TMPDIR:-/tmp}
TEST_DIR=$(mktemp -d "$TMP_BASE/nixsk-tests.XXXXXX")

SOCKET_DIR="$TEST_DIR/sockets"
TEST_HOME="$TEST_DIR/home"
OUTPUT_FILE="$TEST_DIR/output"
ERROR_FILE="$TEST_DIR/error"

NIXSK_BIN="$PROJECT_DIR/build/nixsk"

PASSED=0
FAILED=0

SOCKETS="
nixsk-test.sock
nixsk-test-2.sock
nixsk-parallel-1.sock
nixsk-parallel-2.sock
"

# --------------------------------------------------
# Output
# --------------------------------------------------

pass() {
    printf '[PASS] %s\n' "$1"
    PASSED=$((PASSED + 1))
}

fail() {
    printf '[FAIL] %s\n' "$1"
    FAILED=$((FAILED + 1))
}

section() {
    printf '\n[TEST] %s\n' "$1"
}

# --------------------------------------------------
# Cleanup
# --------------------------------------------------

cleanup() {
    if [ -x "$NIXSK_BIN" ]; then
        for socket in $SOCKETS; do
            NIXSK_HOME="$TEST_HOME" \
                "$NIXSK_BIN" remove --sock="$socket" \
                >/dev/null 2>&1 || true
        done
    fi

    rm -rf "$TEST_DIR"
}

trap cleanup 0
trap 'exit 1' HUP INT TERM

# --------------------------------------------------
# Helpers
# --------------------------------------------------

run_nixsk() {
    NIXSK_HOME="$TEST_HOME" "$NIXSK_BIN" "$@"
}

wait_for_socket() {
    path=$1
    attempts=0

    while [ "$attempts" -lt 50 ]; do
        if [ -S "$path" ]; then
            return 0
        fi

        sleep 0.1
        attempts=$((attempts + 1))
    done

    return 1
}

socket_is_listed() {
    name=$1

    run_nixsk list 2>&1 | grep -F "$name" >/dev/null
}

test_silent_success() {
    description=$1
    shift

    if "$@" >"$OUTPUT_FILE" 2>"$ERROR_FILE"; then
        if [ ! -s "$OUTPUT_FILE" ]; then
            pass "$description"
        else
            fail "$description (unexpected stdout)"
            cat "$OUTPUT_FILE"
            return 1
        fi
    else
        fail "$description (command failed)"
        cat "$ERROR_FILE"
        return 1
    fi
}

# --------------------------------------------------
# Header
# --------------------------------------------------

printf '\n%s\n' "$TEST_NAME"
printf '%s\n\n' '========================================'

mkdir -p "$SOCKET_DIR" "$TEST_HOME"

export NIXSK_HOME="$TEST_HOME"

# --------------------------------------------------
# Dependencies
# --------------------------------------------------

section "Checking dependencies"

if ! command -v bmake >/dev/null 2>&1; then
    printf '[ERROR] bmake is required.\n'
    exit 1
fi

if ! command -v socat >/dev/null 2>&1; then
    printf '[ERROR] socat is required.\n'
    exit 1
fi

pass "bmake available"
pass "socat available"

# --------------------------------------------------
# Build
# --------------------------------------------------

section "Building nixsk"

if (
    cd "$PROJECT_DIR"
    bmake clean && bmake
); then
    if [ -x "$NIXSK_BIN" ]; then
        pass "Build with bmake"
    else
        fail "Build output not found at build/nixsk"
        exit 1
    fi
else
    fail "Build with bmake"
    exit 1
fi

# --------------------------------------------------
# Basic creation
# --------------------------------------------------

section "Socket creation"

test_silent_success \
    "Create socket without stdout output" \
    run_nixsk create \
    --sock=nixsk-test.sock "$SOCKET_DIR"

if wait_for_socket "$SOCKET_DIR/nixsk-test.sock"; then
    pass "Socket file created"
else
    fail "Socket file created"
    exit 1
fi

# --------------------------------------------------
# Registry discovery
# --------------------------------------------------

section "Registry discovery"

if socket_is_listed "nixsk-test.sock"; then
    pass "Managed socket appears in list"
else
    fail "Managed socket appears in list"
    exit 1
fi

# --------------------------------------------------
# Echo connectivity
# --------------------------------------------------

section "Echo server connectivity"

EXPECTED="nixsk-integration-test"
SOCKET_PATH="$SOCKET_DIR/nixsk-test.sock"

if RESPONSE=$(
    printf '%s\n' "$EXPECTED" |
        socat -T 3 - "UNIX-CONNECT:$SOCKET_PATH"
); then
    if [ "$RESPONSE" = "$EXPECTED" ]; then
        pass "Echo server returns the sent message"
    else
        fail "Echo server returned unexpected data"
        printf 'Expected: %s\n' "$EXPECTED"
        printf 'Received: %s\n' "$RESPONSE"
        exit 1
    fi
else
    fail "Echo server connectivity"
    exit 1
fi

# --------------------------------------------------
# Duplicate socket
# --------------------------------------------------

section "Duplicate socket rejection"

if run_nixsk create \
    --sock=nixsk-test.sock "$SOCKET_DIR" \
    >"$OUTPUT_FILE" 2>"$ERROR_FILE"; then

    fail "Duplicate socket rejected"
    exit 1
else
    pass "Duplicate socket rejected"
fi

# Ensure original socket still exists.
if [ -S "$SOCKET_PATH" ]; then
    pass "Original socket preserved after duplicate attempt"
else
    fail "Original socket preserved after duplicate attempt"
    exit 1
fi

# --------------------------------------------------
# Invalid socket name
# --------------------------------------------------

section "Invalid input handling"

if run_nixsk create \
    --sock=../invalid.sock "$SOCKET_DIR" \
    >"$OUTPUT_FILE" 2>"$ERROR_FILE"; then

    fail "Path traversal socket name rejected"
    exit 1
else
    pass "Path traversal socket name rejected"
fi

# --------------------------------------------------
# Multiple sockets
# --------------------------------------------------

section "Multiple socket management"

test_silent_success \
    "Create second socket" \
    run_nixsk create \
    --sock=nixsk-test-2.sock "$SOCKET_DIR"

if wait_for_socket "$SOCKET_DIR/nixsk-test-2.sock"; then
    pass "Second socket file created"
else
    fail "Second socket file created"
    exit 1
fi

if socket_is_listed "nixsk-test-2.sock"; then
    pass "Second socket registered"
else
    fail "Second socket registered"
    exit 1
fi

if socket_is_listed "nixsk-test.sock" &&
   socket_is_listed "nixsk-test-2.sock"; then
    pass "Both sockets listed simultaneously"
else
    fail "Both sockets listed simultaneously"
    exit 1
fi

# --------------------------------------------------
# Concurrent socket creation
# --------------------------------------------------

section "Concurrent socket creation"

run_nixsk create \
    --sock=nixsk-parallel-1.sock "$SOCKET_DIR" \
    >"$TEST_DIR/parallel-1.out" 2>"$TEST_DIR/parallel-1.err" &
PID1=$!

run_nixsk create \
    --sock=nixsk-parallel-2.sock "$SOCKET_DIR" \
    >"$TEST_DIR/parallel-2.out" 2>"$TEST_DIR/parallel-2.err" &
PID2=$!

if wait "$PID1"; then
    RESULT1=0
else
    RESULT1=1
fi

if wait "$PID2"; then
    RESULT2=0
else
    RESULT2=1
fi

if [ "$RESULT1" -eq 0 ] && [ "$RESULT2" -eq 0 ]; then
    pass "Concurrent create commands"
else
    fail "Concurrent create commands"
    cat "$TEST_DIR/parallel-1.err" "$TEST_DIR/parallel-2.err"
    exit 1
fi

if wait_for_socket "$SOCKET_DIR/nixsk-parallel-1.sock" &&
   wait_for_socket "$SOCKET_DIR/nixsk-parallel-2.sock"; then
    pass "Both concurrent sockets created"
else
    fail "Both concurrent sockets created"
    exit 1
fi

if socket_is_listed "nixsk-parallel-1.sock" &&
   socket_is_listed "nixsk-parallel-2.sock"; then
    pass "Concurrent registry updates preserved"
else
    fail "Concurrent registry updates preserved"
    exit 1
fi

# --------------------------------------------------
# Nonexistent socket removal
# --------------------------------------------------

section "Invalid removal handling"

if run_nixsk remove \
    --sock=nixsk-does-not-exist.sock \
    >"$OUTPUT_FILE" 2>"$ERROR_FILE"; then

    fail "Removal of nonexistent socket rejected"
else
    pass "Removal of nonexistent socket rejected"
fi

# --------------------------------------------------
# Socket removal
# --------------------------------------------------

section "Socket removal"

test_silent_success \
    "Remove first socket without stdout output" \
    run_nixsk remove --sock=nixsk-test.sock

if [ ! -S "$SOCKET_DIR/nixsk-test.sock" ]; then
    pass "First socket file removed"
else
    fail "First socket file removed"
    exit 1
fi

if socket_is_listed "nixsk-test.sock"; then
    fail "First socket removed from registry"
    exit 1
else
    pass "First socket removed from registry"
fi

# --------------------------------------------------
# Remove remaining sockets
# --------------------------------------------------

section "Multiple socket removal"

for socket in \
    nixsk-test-2.sock \
    nixsk-parallel-1.sock \
    nixsk-parallel-2.sock
do
    if run_nixsk remove --sock="$socket" \
        >"$OUTPUT_FILE" 2>"$ERROR_FILE"; then

        pass "Remove $socket"
    else
        fail "Remove $socket"
        cat "$ERROR_FILE"
        exit 1
    fi

    if [ ! -S "$SOCKET_DIR/$socket" ]; then
        pass "$socket filesystem cleanup"
    else
        fail "$socket filesystem cleanup"
        exit 1
    fi
done

# --------------------------------------------------
# Registry integrity
# --------------------------------------------------

section "Registry integrity"

if run_nixsk list >"$OUTPUT_FILE" 2>"$ERROR_FILE"; then
    pass "Registry readable after multiple operations"
else
    fail "Registry readable after multiple operations"
    cat "$ERROR_FILE"
    exit 1
fi

if grep -E \
    'nixsk-test\.sock|nixsk-test-2\.sock|nixsk-parallel-1\.sock|nixsk-parallel-2\.sock' \
    "$OUTPUT_FILE" >/dev/null; then

    fail "Removed sockets absent from registry listing"
else
    pass "Removed sockets absent from registry listing"
fi

# --------------------------------------------------
# Summary
# --------------------------------------------------

printf '\n%s\n' '========================================'
printf 'Passed: %s\n' "$PASSED"
printf 'Failed: %s\n' "$FAILED"
printf '%s\n' '========================================'

if [ "$FAILED" -eq 0 ]; then
    printf '\n%s\n' 'ALL TESTS PASSED'
    exit 0
fi

printf '\n%s\n' 'SOME TESTS FAILED'
exit 1
