#!/bin/sh
# Lifecycle of the dwmsysmon sampler against real X servers and the test dwm:
# dwm starts its display's sampler while the section is shown and the sampler
# stops when it is hidden, one sampler per display that never disturbs another
# display's sampler, and no sampler left running when dwm goes away while X
# stays up.
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
tmp=$(cd "$tmp" && pwd -P) # as /proc names the sampler's executable
pids=
cleanup() {
	for pid in $pids; do kill -KILL "$pid" 2>/dev/null || true; done
	# the samplers dwm started, including one a failed check left stopped
	for proc in /proc/[0-9]*; do
		[ "$(readlink "$proc/exe" 2>/dev/null)" = "$tmp/dwmsysmon" ] && kill -KILL "${proc#/proc/}" 2>/dev/null
	done
	rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

fail() {
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

# A sampler that reads a stat file this script keeps advancing at 50% busy,
# and an empty sysfs tree instead of the host's sensors.
$cc -std=c99 -pedantic -Wall -Wextra -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700L \
	-DSYSMON_STAT_PATH=\"$tmp/stat\" -DSYSMON_SYS_PATH=\"$tmp/sys\" $(pkg-config --cflags x11) \
	"$root/src/dwmsysmon.c" "$root/src/sensors.c" "$root/src/sysmon.c" $(pkg-config --libs x11) \
	-o "$tmp/dwmsysmon"
# the sampler dwm starts by name is this one, never an installed dwmsysmon
mkdir "$tmp/bin"
ln -s "$tmp/dwmsysmon" "$tmp/bin/dwmsysmon"
PATH=$tmp/bin:$PATH
export PATH
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
	stat=$(cat "/proc/$(pid_of "$1")/stat" 2>/dev/null) || return 1
	case $stat in *') Z'*) return 1 ;; esac
}
wait_gone() {
	for i in $(seq 1 100); do
		alive "$1" || return 0
		sleep 0.05
	done
	return 1
}
# samplers <display>: the pids of the samplers running on that display, whoever started them
samplers() {
	for proc in /proc/[0-9]*; do
		[ "$(readlink "$proc/exe" 2>/dev/null)" = "$tmp/dwmsysmon" ] || continue
		tr '\0' '\n' <"$proc/environ" 2>/dev/null | grep -qx "DISPLAY=$1" && echo "${proc#/proc/}"
	done
	return 0
}
count() {
	set -- $(samplers "$1")
	echo $#
}
# wait_only <display> <pid>: until that sampler is the only one there
wait_only() {
	for i in $(seq 1 100); do
		[ "$(samplers "$1")" = "$2" ] && return 0
		sleep 0.05
	done
	return 1
}
# wait_samplers <display> <count>: until that many samplers run there
wait_samplers() {
	for i in $(seq 1 100); do
		[ "$(count "$1")" -eq "$2" ] && return 0
		sleep 0.05
	done
	return 1
}
value() { # the sampler's value on a display, empty when there is none
	DISPLAY=$1 xprop -root _DWM_SYSMON 2>/dev/null | sed -n 's/.*"\(cpu=[0-9]*\)[ "].*/\1/p'
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
# show <name> <display> <0|1>: hide or show the section of a running dwm, as a reload does
show() {
	resource "$2" "$3"
	kill -USR2 "$(pid_of "$1")"
}

start_x a
start_x b
a=$(display_of a)
b=$(display_of b)

# two X sessions of the same user, each with its dwm; the section is off by default
spawn dwm_a "$a" "$dwm"
spawn dwm_b "$b" "$dwm"
sleep 1
[ "$(count "$a")" -eq 0 ] && [ "$(count "$b")" -eq 0 ] || fail "dwm started a sampler with the section off by default"
[ -z "$(value "$a")" ] && [ -z "$(value "$b")" ] || fail "a value was published with the section off by default"

# showing the section starts that display's sampler
show dwm_a "$a" 1
wait_samplers "$a" 1 || fail "showing the section in session A did not start one sampler"
wait_value "$a" || fail "session A shows no value"
[ "$(value "$a")" = cpu=50 ] || fail "session A shows $(value "$a"), not the fixture's 50%"
[ "$(count "$b")" -eq 0 ] || fail "showing the section in session A started a sampler in session B"
show dwm_b "$b" 1
wait_samplers "$b" 1 || fail "showing the section in session B did not start one sampler"
wait_value "$b" || fail "session B shows no value"
sampler_a=$(samplers "$a")
sampler_b=$(samplers "$b")

# a replacement sampler on B retires B's own and leaves A's alone
spawn sampler_b2 "$b" "$tmp/dwmsysmon"
wait_only "$b" "$(pid_of sampler_b2)" || fail "the previous sampler of session B kept running beside its replacement"
[ "$(samplers "$a")" = "$sampler_a" ] || fail "starting a sampler in session B stopped the sampler of session A"
sleep 1
[ -n "$(value "$a")" ] && [ -n "$(value "$b")" ] || fail "a value is missing after the replacement in session B"

# hiding the section stops B's sampler, whoever started it, and only that one
show dwm_b "$b" 0
wait_samplers "$b" 0 || fail "hiding the section in session B left its sampler running"
wait_no_value "$b" || fail "the sampler of session B left its value behind when the section was hidden"
[ "$(samplers "$a")" = "$sampler_a" ] || fail "hiding the section in session B stopped the sampler of session A"
[ -n "$(value "$a")" ] || fail "hiding the section in session B removed the value of session A"
# a sampler started while the section is hidden does not stay
spawn sampler_b3 "$b" "$tmp/dwmsysmon"
wait_gone sampler_b3 || fail "a sampler started while the section is hidden kept running"
[ -z "$(value "$b")" ] || fail "a sampler started while the section is hidden published a value"
# showing it again starts a new one and the value comes back
show dwm_b "$b" 1
wait_samplers "$b" 1 || fail "showing the section in session B again did not start one sampler"
wait_value "$b" || fail "session B shows no value after the section was shown again"
[ "$(samplers "$a")" = "$sampler_a" ] || fail "showing the section in session B again disturbed the sampler of session A"

# a sampler that misses the stop is retired by the next one dwm starts
stuck=$(samplers "$b")
kill -STOP "$stuck"
show dwm_b "$b" 0
sleep 0.5
alive dwm_b || fail "dwm of session B died beside a stuck sampler"
show dwm_b "$b" 1
wait_samplers "$b" 2 || fail "dwm of session B did not start a sampler beside the stuck one"
kill -CONT "$stuck"
wait_samplers "$b" 1 && [ "$(samplers "$b")" != "$stuck" ] || fail "the stuck sampler of session B was not retired"
wait_value "$b" || fail "session B shows no value after its stuck sampler was retired"
[ "$(samplers "$a")" = "$sampler_a" ] || fail "retiring the stuck sampler of session B disturbed session A"

# restarting dwm in B: the sampler goes with its dwm, A is untouched
kill -TERM "$(pid_of dwm_b)"
wait_gone dwm_b || fail "dwm of session B did not exit"
wait_samplers "$b" 0 || fail "the sampler of session B outlived its dwm while the X server stayed up"
wait_no_value "$b" || fail "the sampler of session B left its value behind"
[ "$(samplers "$a")" = "$sampler_a" ] || fail "restarting dwm in session B stopped the sampler of session A"
[ -n "$(value "$a")" ] || fail "restarting dwm in session B removed the value of session A"
# shown by the resource at start-up: dwm starts the sampler right away
spawn dwm_b "$b" "$dwm"
wait_samplers "$b" 1 || fail "dwm did not start a sampler for a section shown at start-up"
wait_value "$b" || fail "session B shows no value after its dwm restarted"
[ "$(samplers "$a")" = "$sampler_a" ] && [ -n "$(value "$a")" ] || fail "session A lost its sampler or value during B's restart"

# hidden by the resource at start-up: no sampler at all
kill -TERM "$(pid_of dwm_b)"
wait_gone dwm_b || fail "dwm of session B did not exit"
wait_samplers "$b" 0 || fail "the sampler of session B did not exit with its dwm"
resource "$b" 0
spawn dwm_b "$b" "$dwm"
sleep 1
[ "$(count "$b")" -eq 0 ] || fail "dwm started a sampler for a section hidden at start-up"
[ -z "$(value "$b")" ] || fail "a hidden section has a value"
kill -TERM "$(pid_of dwm_b)"
wait_gone dwm_b || fail "dwm of session B did not exit"

# dwm killed outright, not exiting normally
resource "$b" 1
spawn dwm_b "$b" "$dwm"
wait_samplers "$b" 1 || fail "dwm of session B did not start its sampler"
wait_value "$b" || fail "session B shows no value"
kill -KILL "$(pid_of dwm_b)"
wait_samplers "$b" 0 || fail "the sampler outlived a dwm that was killed"

# a sampler started when dwm is gone does not linger
spawn sampler_b6 "$b" "$tmp/dwmsysmon"
wait_gone sampler_b6 || fail "a sampler started after dwm was gone kept running"

# and A, whose dwm exits normally last, is cleaned up the same way
[ "$(samplers "$a")" = "$sampler_a" ] || fail "session A lost its sampler"
kill -TERM "$(pid_of dwm_a)"
wait_gone dwm_a || fail "dwm of session A did not exit"
wait_samplers "$a" 0 || fail "the sampler of session A outlived its dwm"
wait_no_value "$a" || fail "the sampler of session A left its value behind"

printf '%s\n' 'system monitor lifecycle checks passed'
