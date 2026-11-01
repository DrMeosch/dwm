#!/bin/sh
# Lifecycle of the dwmsysmon sampler against real X servers and the test dwm:
# one sampler per display that never disturbs another display's sampler, and no
# sampler left running when dwm goes away while X stays up.
#
# usage: sysmon-lifecycle.sh <repository root> <dwm built with -DDWM_TEST>
set -eu

root=$1
dwm=$2
cc=${CC:-cc}

missing=
for tool in Xvfb xrdb xprop pkg-config; do
	command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
if [ -n "$missing" ]; then
	printf 'system monitor lifecycle checks skipped; missing:%s\n' "$missing"
	exit 0
fi

tmp=$(mktemp -d)
pids=
cleanup() {
	for pid in $pids; do kill -KILL "$pid" 2>/dev/null || true; done
	rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

fail() {
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

# A sampler that reads a stat file this script keeps advancing at 50% busy.
$cc -std=c99 -pedantic -Wall -Wextra -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700L \
	-DSYSMON_STAT_PATH=\"$tmp/stat\" $(pkg-config --cflags x11) \
	"$root/src/dwmsysmon.c" "$root/src/sysmon.c" $(pkg-config --libs x11) \
	-o "$tmp/dwmsysmon"
(
	n=0
	while :; do
		n=$((n + 1))
		printf 'cpu  %d 0 0 %d 0 0 0 0\n' $((n * 5)) $((n * 5)) >"$tmp/stat.new"
		mv "$tmp/stat.new" "$tmp/stat"
		sleep 0.1
	done
) &
pids="$pids $!"

# start_x <name>: a private X server whose display is left in $tmp/<name>.display
start_x() {
	Xvfb -displayfd 3 -screen 0 1000x300x24 -nolisten tcp -noreset \
		>"$tmp/$1.log" 2>&1 3>"$tmp/$1.display" &
	pids="$pids $!"
	for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
		[ -s "$tmp/$1.display" ] && return 0
		sleep 0.1
	done
	fail "X server $1 did not start"
}
display_of() {
	printf ':%s' "$(cat "$tmp/$1.display")"
}

# spawn <name> <display> <command...>: run in the background; the pid goes to $tmp/<name>.pid
spawn() {
	name=$1
	disp=$2
	shift 2
	DISPLAY=$disp "$@" >"$tmp/$name.log" 2>&1 &
	echo $! >"$tmp/$name.pid"
	pids="$pids $!"
}
pid_of() {
	cat "$tmp/$1.pid"
}
alive() {
	[ -r "/proc/$(pid_of "$1")/stat" ] && ! grep -q ') Z' "/proc/$(pid_of "$1")/stat"
}
wait_gone() {
	for i in $(seq 1 100); do
		alive "$1" || return 0
		sleep 0.05
	done
	return 1
}
value() { # the sampler's value on a display, empty when there is none
	DISPLAY=$1 xprop -root _DWM_SYSMON 2>/dev/null | sed -n 's/.*"\(cpu=[0-9]*\)".*/\1/p'
}
wait_value() {
	for i in $(seq 1 100); do
		[ -n "$(value "$1")" ] && return 0
		sleep 0.1
	done
	return 1
}
wait_no_value() {
	for i in $(seq 1 100); do
		[ -z "$(value "$1")" ] && return 0
		sleep 0.05
	done
	return 1
}
resource() {
	printf 'dwm.showsysmon: %s\n' "$2" | DISPLAY=$1 xrdb -merge
}

start_x a
start_x b
a=$(display_of a)
b=$(display_of b)

# two X sessions of the same user, each with its dwm and sampler
resource "$a" 1
resource "$b" 1
spawn dwm_a "$a" "$dwm"
spawn dwm_b "$b" "$dwm"
sleep 0.5
spawn sampler_a "$a" "$tmp/dwmsysmon"
spawn sampler_b1 "$b" "$tmp/dwmsysmon"
wait_value "$a" || fail "session A shows no value"
wait_value "$b" || fail "session B shows no value"
[ "$(value "$a")" = cpu=50 ] || fail "session A shows $(value "$a"), not the fixture's 50%"

# a replacement sampler on B retires B's own and leaves A's alone
spawn sampler_b2 "$b" "$tmp/dwmsysmon"
wait_gone sampler_b1 || fail "the previous sampler of session B kept running beside its replacement"
alive sampler_b2 || fail "the replacement sampler of session B is not running"
alive sampler_a || fail "starting a sampler in session B stopped the sampler of session A"
sleep 1
[ -n "$(value "$a")" ] && [ -n "$(value "$b")" ] || fail "a value is missing after the replacement in session B"

# restarting dwm in B: the sampler goes with its dwm, A is untouched
kill -TERM "$(pid_of dwm_b)"
wait_gone dwm_b || fail "dwm of session B did not exit"
wait_gone sampler_b2 || fail "the sampler of session B outlived its dwm while the X server stayed up"
wait_no_value "$b" || fail "the sampler of session B left its value behind"
alive sampler_a || fail "restarting dwm in session B stopped the sampler of session A"
[ -n "$(value "$a")" ] || fail "restarting dwm in session B removed the value of session A"
spawn dwm_b "$b" "$dwm"
sleep 0.5
spawn sampler_b3 "$b" "$tmp/dwmsysmon"
wait_value "$b" || fail "session B shows no value after its dwm restarted"
alive sampler_a && [ -n "$(value "$a")" ] || fail "session A lost its sampler or value during B's restart"

# the section hidden: the idle sampler goes with dwm as well
kill -TERM "$(pid_of dwm_b)"
wait_gone dwm_b || fail "dwm of session B did not exit"
wait_gone sampler_b3 || fail "the sampler of session B did not exit with its dwm"
resource "$b" 0
spawn dwm_b "$b" "$dwm"
sleep 0.5
spawn sampler_b4 "$b" "$tmp/dwmsysmon"
sleep 1.5
[ -z "$(value "$b")" ] || fail "a hidden section has a value"
alive sampler_b4 || fail "the sampler of a hidden section is not running"
kill -TERM "$(pid_of dwm_b)"
wait_gone dwm_b || fail "dwm of session B did not exit"
wait_gone sampler_b4 || fail "the sampler of a hidden section outlived its dwm while the X server stayed up"

# dwm killed outright, not exiting normally
resource "$b" 1
spawn dwm_b "$b" "$dwm"
sleep 0.5
spawn sampler_b5 "$b" "$tmp/dwmsysmon"
wait_value "$b" || fail "session B shows no value"
kill -KILL "$(pid_of dwm_b)"
wait_gone sampler_b5 || fail "the sampler outlived a dwm that was killed"

# a sampler started when dwm is gone does not linger
spawn sampler_b6 "$b" "$tmp/dwmsysmon"
wait_gone sampler_b6 || fail "a sampler started after dwm was gone kept running"

# and A, whose dwm exits normally last, is cleaned up the same way
alive sampler_a || fail "session A lost its sampler"
kill -TERM "$(pid_of dwm_a)"
wait_gone dwm_a || fail "dwm of session A did not exit"
wait_gone sampler_a || fail "the sampler of session A outlived its dwm"
wait_no_value "$a" || fail "the sampler of session A left its value behind"

printf '%s\n' 'system monitor lifecycle checks passed'
