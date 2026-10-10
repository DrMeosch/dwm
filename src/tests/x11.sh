#!/bin/sh
set -eu

root=$1
cc=${CC:-cc}

if ! command -v Xvfb >/dev/null 2>&1 \
|| ! command -v xdpyinfo >/dev/null 2>&1 \
|| ! command -v xrdb >/dev/null 2>&1; then
	if [ -n "${DWM_X11_CAPTURE_DIR:-}" ]; then
		printf '%s\n' 'X11 screenshot capture requires Xvfb, xdpyinfo, and xrdb'
		exit 1
	fi
	printf '%s\n' 'X11 integration checks skipped: Xvfb, xdpyinfo, and xrdb are required'
	exit 0
fi

tmp=$(mktemp -d)
display=:91
server_pid=
dwm_pid=
transient_pid=
capture_pid=
cleanup() {
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
$cc -std=c99 -pedantic -Wall -Wextra $flags "$root/src/tests/x11.c" \
	$libs -o "$tmp/x11"
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

"$tmp/dwm" >"$tmp/dwm.log" 2>&1 &
dwm_pid=$!
sleep 0.2
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

printf '%s\n' 'X11 integration checks passed'
