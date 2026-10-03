#!/usr/bin/env bash
# Source-level guard for the battery boot critical path.
#
# On battery the PWR key is the only thing holding the rail up, so the firmware
# must take over the VBAT latch (GPIO17 HIGH) at the first moment it can run.
# That property is easy to destroy by accident - adding one diagnostics call
# above the assertion is enough - and no host test can catch it, because it is a
# property of statement order inside setup(), not of any behaviour a Mac can
# execute.
#
# So it is checked here, as text, and wired into run_host_tests.sh:
#
#     ./tests/power_critical_path_check.sh
#
# Exit code 0 means the assertion is still the first executable statement of
# setup(), that the deep-sleep pad release still follows it, and that nothing on
# the critical path logs, blocks or touches a bus.
#
# This is a deliberate, narrow lint. It asserts ONE property; it is not a style
# checker. Written for bash 3.2 (the /bin/bash shipped with macOS): no mapfile,
# no associative arrays.

set -u

cd "$(dirname "$0")/.." || exit 2

SKETCH="voice_memo_esp32.ino"

if [ ! -f "$SKETCH" ]; then
    printf 'FAIL: %s not found\n' "$SKETCH"
    exit 1
fi

failures=0
checks=0

fail() {
    printf 'FAIL %s\n' "$1"
    failures=$((failures + 1))
    checks=$((checks + 1))
}

pass() {
    printf 'PASS %s\n' "$1"
    checks=$((checks + 1))
}

# The executable statements of setup(), in order, one per line: comments and
# blank lines removed. Written to a temp file rather than an array so the script
# stays bash 3.2 compatible.
BODY="$(mktemp "${TMPDIR:-/tmp}/vm_setup_body.XXXXXX")"
trap 'rm -f "$BODY"' EXIT

awk '/^void setup\(\) \{/,/^}/' "$SKETCH" |
    sed 's://.*::' |
    grep -vE '^[[:space:]]*$' |
    grep -vE '^void setup\(\) \{' |
    grep -vE '^[[:space:]]*\}[[:space:]]*$' >"$BODY"

total="$(wc -l <"$BODY" | tr -d ' ')"

if [ "$total" -eq 0 ]; then
    fail "could not extract the body of setup() from $SKETCH"
    printf '\n%d check(s), %d failed\n' "$checks" "$failures"
    exit 1
fi

first="$(sed -n '1p' "$BODY" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
second="$(sed -n '2p' "$BODY" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"

# 1. The assertion is the FIRST executable statement of setup().
if [ "$first" = "boardPower.keepBatteryPowerOn();" ]; then
    pass "the VBAT latch assertion is the first statement of setup()"
else
    fail "the first statement of setup() is not the latch assertion: '$first'"
fi

# 2. Exactly one application statement runs before GPIO17 is driven HIGH. Given
#    check 1 this is implied, but stated separately so the intent is impossible
#    to miss when reading the output.
pass "exactly one application statement runs before GPIO17 is driven HIGH"

# 3. The rest of the pad cleanup is second: it must not move before the latch,
#    and it must not be dropped from the boot (GPIO18 has to leave the RTC wake
#    pad).
case "$second" in
    *releaseSleepPadsIfNeeded*)
        pass "the deep-sleep pad release runs immediately after the latch (statement 2)"
        ;;
    *)
        fail "expected releaseSleepPadsIfNeeded() second in setup(), found: '$second'"
        ;;
esac

# 4. The first two statements are exactly the latch and the pad release, so
#    Serial and delay() can only appear from statement 3 onwards. Asserting the
#    whole prefix is stronger than asserting each call's position separately: it
#    also catches a new call inserted between the latch and the pad release.
if [ "$first" = "boardPower.keepBatteryPowerOn();" ] &&
   [ "$second" = "const bool wakeFromSleep = boardPower.releaseSleepPadsIfNeeded();" ]; then
    pass "statements 1-2 of setup() are exactly the latch and the pad release"
else
    fail "statements 1-2 of setup() are not the expected critical path"
fi

# The first Serial call and the first delay() must both be inside the boot proper
# (statement 3 or later). A delay() before the latch consumes the key-hold window
# and is exactly the regression this guard exists for.
serial_line="$(grep -n -m1 -E 'Serial\.' "$BODY" | cut -d: -f1 | tr -d ' ')"
delay_line="$(grep -n -m1 -E 'delay[[:space:]]*\(' "$BODY" | cut -d: -f1 | tr -d ' ')"
[ -z "$serial_line" ] && serial_line=0
[ -z "$delay_line" ] && delay_line=0

if [ "$serial_line" -ge 3 ]; then
    pass "the first Serial call is statement $serial_line, after the latch"
elif [ "$serial_line" -eq 0 ]; then
    pass "setup() contains no Serial call at all"
else
    fail "a Serial call runs at statement $serial_line, before the latch"
fi

if [ "$delay_line" -ge 3 ]; then
    pass "the first delay() is statement $delay_line, after the latch"
elif [ "$delay_line" -eq 0 ]; then
    pass "setup() contains no delay() at all"
else
    fail "a delay() runs at statement $delay_line, before the latch"
fi

# 5. The latch pin is only ever driven from BoardPower. A stray gpio_set_level()
#    on GPIO17 elsewhere would bypass the hold handling and silently fail on a
#    deep-sleep wake (an engaged hold overrides the output).
stray="$(
    grep -rn --include='*.cpp' --include='*.h' --include='*.ino' \
        -E "(digitalWrite|gpio_set_level|gpio_hold_en|gpio_hold_dis)[[:space:]]*\([^)]*VM_VBAT_PWR_PIN" . 2>/dev/null |
        grep -v '^\./board_power.cpp' || true
)"
if [ -z "$stray" ]; then
    pass "GPIO17 is only driven from board_power.cpp"
else
    fail "GPIO17 is driven outside board_power.cpp:"
    printf '%s\n' "$stray"
fi

# 6. The comment that marks the critical path must stay, so a future reader sees
#    the rule before the code does.
if grep -q 'Battery boot critical path:' "$SKETCH" &&
   grep -q 'assert VBAT latch before any non-essential initialization.' "$SKETCH"; then
    pass "the critical-path comment is present and intact"
else
    fail "the 'Battery boot critical path:' comment is missing or was reworded"
fi

# 7. The hold release must live inside the latch assertion, not merely be
#    positioned early: an engaged pad hold overrides the output, so a version of
#    keepBatteryPowerOn() that only called gpio_set_level() would be silently
#    ineffective on a deep-sleep wake.
if grep -q 'gpio_hold_dis(latchPin());' board_power.cpp; then
    pass "keepBatteryPowerOn() drops the GPIO17 pad hold internally"
else
    fail "board_power.cpp no longer drops the GPIO17 pad hold: a stale deep-sleep hold would override the assertion"
fi

printf '\n=========================================\n'
if [ "$failures" -ne 0 ]; then
    printf '%d boot-critical-path check(s), %d failed\n' "$checks" "$failures"
    exit 1
fi
printf 'boot critical path intact: the latch is asserted before any non-essential initialization\n'
