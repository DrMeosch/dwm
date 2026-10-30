#!/bin/sh
set -eu

root=$1
cc=${CC:-cc}
missing_deps=

if ! command -v Xvfb >/dev/null 2>&1; then missing_deps="$missing_deps Xvfb"; fi
if ! command -v xdpyinfo >/dev/null 2>&1; then missing_deps="$missing_deps xdpyinfo"; fi
if ! command -v xrdb >/dev/null 2>&1; then missing_deps="$missing_deps xrdb"; fi
if ! command -v pkg-config >/dev/null 2>&1; then
	missing_deps="$missing_deps pkg-config"
fi
if [ -n "$missing_deps" ]; then
	if [ -n "${DWM_X11_CAPTURE_DIR:-}" ]; then
		printf 'X11 workflow cannot run; missing:%s\n' "$missing_deps"
		exit 1
	fi
	printf 'X11 integration checks skipped; missing:%s\n' "$missing_deps"
	exit 0
fi

input_available=1
if ! pkg-config --exists xtst; then
	input_available=0
	printf '%s\n' 'X11 input regression check skipped; XTEST development files are unavailable'
fi

tmp=$(mktemp -d)
display=:91
server_pid=
dwm_pid=
transient_pid=
capture_pid=
fake_pid=
cleanup() {
	if [ -n "$fake_pid" ]; then kill -TERM "$fake_pid" 2>/dev/null || true; fi
	if [ -n "$capture_pid" ]; then kill -TERM "$capture_pid" 2>/dev/null || true; fi
	if [ -n "$transient_pid" ]; then kill -TERM "$transient_pid" 2>/dev/null || true; fi
	if [ -n "$dwm_pid" ]; then kill -TERM "$dwm_pid" 2>/dev/null || true; fi
	if [ -n "$server_pid" ]; then kill -TERM "$server_pid" 2>/dev/null || true; fi
}
trap cleanup EXIT HUP INT TERM

flags=$(pkg-config --cflags x11 xft fontconfig x11-xcb xcb xcb-res xinerama)
libs=$(pkg-config --libs x11 xft fontconfig x11-xcb xcb xcb-res xinerama)
$cc -std=c99 -pedantic -Wall -Wextra -Wno-unused-function \
	-Wno-deprecated-declarations -DDWM_TEST -I"$root/src" \
	-D_DEFAULT_SOURCE -D_BSD_SOURCE -D_XOPEN_SOURCE=700L -DXINERAMA \
	-DVERSION=\"6.5\" $flags "$root/src/dwm.c" "$root/src/drw.c" \
	"$root/src/borderwidth.c" "$root/src/selborder.c" \
	"$root/src/sigusr2.c" "$root/src/util.c" \
	$libs -lm -o "$tmp/dwm"
$cc -std=c99 -pedantic -Wall -Wextra -Wno-unused-function \
	-Wno-unused-parameter -Wno-sign-compare \
	-Wno-deprecated-declarations -DDWM_TEST -I"$root/src" \
	-D_DEFAULT_SOURCE -D_BSD_SOURCE -D_XOPEN_SOURCE=700L -DXINERAMA \
	-DVERSION=\"6.5\" $flags "$root/src/tests/eventloop.c" "$root/src/drw.c" \
	"$root/src/borderwidth.c" "$root/src/selborder.c" \
	"$root/src/sigusr2.c" "$root/src/util.c" \
	$libs -lm -o "$tmp/eventloop"
$cc -std=c99 -pedantic -Wall -Wextra $flags "$root/src/tests/x11.c" \
	$libs -o "$tmp/x11"
if [ "$input_available" -eq 1 ]; then
	input_flags=$(pkg-config --cflags xtst)
	input_libs=$(pkg-config --libs xtst)
	$cc -std=c99 -pedantic -Wall -Wextra $flags $input_flags \
		"$root/src/tests/input.c" $libs $input_libs -o "$tmp/input"
	$cc -std=c99 -pedantic -Wall -Wextra $flags $input_flags \
		"$root/src/tests/statusclick.c" $libs $input_libs -o "$tmp/statusclick"
fi
$cc -std=c99 -pedantic -Wall -Wextra $flags "$root/src/transient.c" \
	$libs -o "$tmp/transient"
if [ -n "${DWM_X11_CAPTURE_DIR:-}" ]; then
	command -v python3 >/dev/null 2>&1 || {
		printf '%s\n' 'X11 screenshot capture requires python3'
		exit 1
	}
	mkdir -p "$DWM_X11_CAPTURE_DIR"
fi
$cc -std=c99 -pedantic -Wall -Wextra -D_DEFAULT_SOURCE \
	"$root/src/tests/x11-capture.c" $libs -o "$tmp/x11-capture"

Xvfb "$display" -screen 0 1200x800x24 -nolisten tcp -noreset >"$tmp/xvfb.log" 2>&1 &
server_pid=$!
export DISPLAY=$display
for i in 1 2 3 4 5 6 7 8 9 10; do
	if xdpyinfo >/dev/null 2>&1; then break; fi
	sleep 0.1
done
xdpyinfo >/dev/null 2>&1
printf 'dwm.borderpx: 1\ndwm.selbordercolor: #ffb86c\n' | xrdb -merge
if [ -n "${DWM_X11_CAPTURE_DIR:-}" ]; then
	"$tmp/x11-capture" root '#282a36'
fi

"$tmp/eventloop"

"$tmp/dwm" >"$tmp/dwm.log" 2>&1 &
dwm_pid=$!
sleep 0.2
if [ "$input_available" -eq 1 ]; then
	"$tmp/input"
	# dwm finds dwmblocks with pidof; never signal a real one
	if ! command -v pidof >/dev/null 2>&1; then
		printf '%s\n' 'status click regression checks skipped; pidof is unavailable'
	elif pidof dwmblocks >/dev/null 2>&1; then
		printf '%s\n' 'status click regression checks skipped; a real dwmblocks is running'
	else
		mkdir "$tmp/fake"
		cp "$tmp/statusclick" "$tmp/fake/dwmblocks"
		"$tmp/fake/dwmblocks" fake "$tmp/dwmblocks.log" &
		fake_pid=$!
		for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
			if [ -s "$tmp/dwmblocks.log" ]; then break; fi
			sleep 0.05
		done
		"$tmp/statusclick" "$tmp/dwmblocks.log"
		kill -TERM "$fake_pid" 2>/dev/null || true
		fake_pid=
	fi
fi
"$tmp/transient" >"$tmp/transient.log" 2>&1 &
transient_pid=$!
"$tmp/x11"

window_file=$tmp/issue10-window.id
"$tmp/x11-capture" client "$window_file" >"$tmp/capture-client.log" 2>&1 &
capture_pid=$!
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
	if [ -s "$window_file" ]; then break; fi
	sleep 0.05
done
if [ ! -s "$window_file" ]; then
	cat "$tmp/capture-client.log" >&2
	printf '%s\n' 'X11 reload client did not start'
	exit 1
fi
read -r window_id tagged_window_id < "$window_file"
wait_for_border() {
	expected=$1
	expected_width=$2
	for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
		if "$tmp/x11-capture" check "$window_id" "$expected" "$expected_width" \
			"$tagged_window_id" >"$tmp/border-check.log" 2>&1; then
			cat "$tmp/border-check.log"
			return 0
		fi
		sleep 0.05
	done
	cat "$tmp/border-check.log" >&2
	return 1
}
wait_for_border '#ffb86c' 1
if [ -n "${DWM_X11_CAPTURE_DIR:-}" ]; then
	"$tmp/x11-capture" capture "$tmp/dark.ppm"
	python3 "$root/src/tests/ppm-to-png.py" "$tmp/dark.ppm" \
		"$DWM_X11_CAPTURE_DIR/dark.png"
fi

printf 'dwm.borderpx: 2\ndwm.selbordercolor: #000000\n' | xrdb -merge
kill -USR2 "$dwm_pid"
wait_for_border '#000000' 2
kill -0 "$dwm_pid"
if [ -n "${DWM_X11_CAPTURE_DIR:-}" ]; then
	"$tmp/x11-capture" root '#f0f1f6'
	"$tmp/x11-capture" capture "$tmp/light.ppm"
	python3 "$root/src/tests/ppm-to-png.py" "$tmp/light.ppm" \
		"$DWM_X11_CAPTURE_DIR/light.png"
fi

printf 'dwm.borderpx: invalid\ndwm.selbordercolor: #000000\n' | xrdb -load
kill -USR2 "$dwm_pid"
wait_for_border '#000000' 2

printf 'dwm.borderpx: 1\ndwm.selbordercolor: #000000\n' | xrdb -load
kill -USR2 "$dwm_pid"
wait_for_border '#000000' 1

printf 'dwm.color8: 1\ndwm.selbordercolor: #000000\n' | xrdb -load
kill -USR2 "$dwm_pid"
wait_for_border '#000000' 2

# status2d palette entries follow Xresources on reload; a malformed one keeps its default
"$tmp/x11" bar-color '#50fa7b' 16
"$tmp/x11" bar-color '#f1fa8c' 16
printf 'dwm.color2: #3060ff\ndwm.color3: bogus\n' | xrdb -load
kill -USR2 "$dwm_pid"
"$tmp/x11" bar-color '#3060ff' 16
"$tmp/x11" bar-color '#50fa7b' 0
"$tmp/x11" bar-color '#f1fa8c' 16
printf 'dwm.borderpx: 1\n' | xrdb -load
kill -USR2 "$dwm_pid"
"$tmp/x11" bar-color '#50fa7b' 16
# docs/dwm-{light,dark}.sh must still switch dwm when every desktop step fails,
# and must report the result with notify-send. The stubs log their arguments;
# pkill only ever signals the test dwm, never a real one, and notify-send never
# reaches a real notification daemon.
stubs=$tmp/stubs
mkdir "$stubs"
for tool in xfconf-query gsettings atc; do
	printf '#!/bin/sh\necho "%s $*" >>"$STUB_LOG"\nexit "${STUB_EXIT:-3}"\n' "$tool" >"$stubs/$tool"
	chmod +x "$stubs/$tool"
done
printf '#!/bin/sh\n[ "$*" = "-USR2 -x dwm" ] || exit 2\nexec kill -USR2 "$STUB_DWM_PID"\n' >"$stubs/pkill"
printf '#!/bin/sh\nfor arg; do printf "%%s\\n" "$arg"; done >"$STUB_NOTIFY"\nexit "${STUB_NOTIFY_EXIT:-0}"\n' >"$stubs/notify-send"
chmod +x "$stubs/pkill" "$stubs/notify-send"
theme_fail() {
	printf '%s\n' "$@" >&2
	exit 1
}
# run_theme <light|dark> <desktop step exit> <notify-send exit>
run_theme() {
	: >"$tmp/stub.log"
	rm -f "$tmp/notify.args"
	rc=0
	STUB_LOG=$tmp/stub.log STUB_NOTIFY=$tmp/notify.args STUB_DWM_PID=$dwm_pid \
		STUB_EXIT=$2 STUB_NOTIFY_EXIT=$3 PATH=$stubs:$PATH \
		bash "$root/docs/dwm-$1.sh" 2>"$tmp/theme.err" || rc=$?
	for step in xfconf-query gsettings atc; do
		grep -q "^$step " "$tmp/stub.log" || theme_fail "dwm-$1.sh did not run $step"
	done
	[ -s "$tmp/notify.args" ] || theme_fail "dwm-$1.sh sent no notification"
	# dmenu reads dmenu.color0/4 from Xresources at launch, so the merge must
	# carry the same palette as dwm.color0/4
	for n in 0 4; do
		want=$(xrdb -query | sed -n "s/^dwm\\.color$n:[[:space:]]*//p")
		got=$(xrdb -query | sed -n "s/^dmenu\\.color$n:[[:space:]]*//p")
		[ -n "$want" ] && [ "$got" = "$want" ] ||
			theme_fail "dwm-$1.sh left dmenu.color$n '$got' instead of dwm.color$n '$want'"
	done
}
# every desktop step fails: stderr and the critical notification name all three
failed_report="dwm-%s: xfconf-query failed (exit 3)
dwm-%s: gsettings failed (exit 3)
dwm-%s: atc failed (exit 3)"
for notify_exit in 0 1; do
	for theme in light dark; do
		run_theme "$theme" 3 "$notify_exit"
		[ "$rc" -ne 0 ] || theme_fail "dwm-$theme.sh exited 0 although its desktop steps failed"
		expected=$(printf "$failed_report" "$theme" "$theme" "$theme")
		[ "$(cat "$tmp/theme.err")" = "$expected" ] ||
			theme_fail "dwm-$theme.sh stderr is not the three failed steps:" "$(cat "$tmp/theme.err")"
		[ "$(cat "$tmp/notify.args")" = "-u
critical
dwm theme
$expected" ] ||
			theme_fail "dwm-$theme.sh did not send the critical notification:" "$(cat "$tmp/notify.args")"
		# a notify-send that fails changes neither the status nor the reload
		if [ "$theme" = light ]; then
			wait_for_border '#b84f7b' 2
			"$tmp/x11" bar-background '#f6e8ee'
		else
			wait_for_border '#b84f7b' 1
			"$tmp/x11" bar-background '#282a36'
		fi
	done
done
# every step succeeds: quiet stderr, exit 0 and a low-urgency notification
for theme in dark light; do
	run_theme "$theme" 0 0
	[ "$rc" -eq 0 ] || theme_fail "dwm-$theme.sh failed although every step succeeded"
	[ ! -s "$tmp/theme.err" ] || theme_fail "dwm-$theme.sh wrote to stderr:" "$(cat "$tmp/theme.err")"
	[ "$(cat "$tmp/notify.args")" = "-u
low
dwm theme
Switched to $theme theme" ] ||
		theme_fail "dwm-$theme.sh did not send the low-urgency notification:" "$(cat "$tmp/notify.args")"
done
wait_for_border '#b84f7b' 2
"$tmp/x11" bar-background '#f6e8ee'
kill -0 "$dwm_pid"

printf '%s\n' 'X11 integration checks passed'
