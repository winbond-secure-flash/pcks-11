#!/bin/sh
# run-all-tests.sh - Master test runner for W77Q/TEE/PKCS#11 on-target tests
# Deploy to /usr/bin/run-all-tests.sh on the board.
case "$1" in --help|-h)
    echo "Usage: $(basename $0)"
    echo ""
    echo "  Master runner: executes ft9 through ft22 (all automated on-target tests) in sequence."
    echo ""
    echo "Pass/Fail criteria: Reports PASS/FAIL per test and overall suite result."
    exit 0 ;;
esac
. /usr/bin/tee-tests-common.sh

TOTAL=0
PASSED=0
FAILED=0

run_test() {
    local name="$1"
    local script="$2"
    TOTAL=$((TOTAL + 1))
    echo
    echo "=============================="
    echo " Running $name"
    echo "=============================="
    if sh "$script"; then
        echo "  --> $name: PASS"
        PASSED=$((PASSED + 1))
    else
        echo "  --> $name: FAIL"
        FAILED=$((FAILED + 1))
    fi
}

SCRIPT_DIR=/usr/bin

# ft15 and ft15b are host-only; skip them
run_test  "ft9"  "$SCRIPT_DIR/ft9-target.sh"
run_test "ft10"  "$SCRIPT_DIR/ft10-target.sh"
run_test "ft11"  "$SCRIPT_DIR/ft11-target.sh"
run_test "ft12"  "$SCRIPT_DIR/ft12-target.sh"
run_test "ft13"  "$SCRIPT_DIR/ft13-target.sh"
run_test "ft14"  "$SCRIPT_DIR/ft14-target.sh"
run_test "ft16"  "$SCRIPT_DIR/ft16-target.sh"
run_test "ft17"  "$SCRIPT_DIR/ft17-target.sh"
run_test "ft18"  "$SCRIPT_DIR/ft18-target.sh"
run_test "ft19"  "$SCRIPT_DIR/ft19-target.sh"
run_test "ft20"  "$SCRIPT_DIR/ft20-target.sh"
run_test "ft22"  "$SCRIPT_DIR/ft22-target.sh"
run_test "ft23"  "$SCRIPT_DIR/ft23-target.sh"
run_test "ft24"  "$SCRIPT_DIR/ft24-target.sh"
run_test "ft25"  "$SCRIPT_DIR/ft25-target.sh"

echo
echo "=============================="
echo " Test summary"
echo "=============================="
echo "  Total:  $TOTAL"
echo "  Passed: $PASSED"
echo "  Failed: $FAILED"
echo "=============================="

if [ "$FAILED" -eq 0 ]; then
    echo "  ALL TESTS PASSED"
    exit 0
else
    echo "  $FAILED TEST(S) FAILED"
    exit 1
fi
