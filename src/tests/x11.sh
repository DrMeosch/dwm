#!/bin/sh
set -eu

root=$1
cc=${CC:-cc}

if ! command -v Xvfb >/dev/null 2>&1 \
|| ! command -v xdpyinfo >/dev/null 2>&1 \
|| ! command -v xrdb >/dev/null 2>&1; then
	printf '%s\n' 'X11 integration checks skipped: Xvfb, xdpyinfo, and xrdb are required'
	exit 0
fi

tmp=$(mktemp -d)
display=:91
server_pid=
dwm_pid=
transient_pid=
cleanup() {
	if [ -n "$transient_pid" ]; then kill -TERM "$transient_pid" 2>/dev/null || true; fi
	if [ -n "$dwm_pid" ]; then kill -TERM "$dwm_pid" 2>/dev/null || true; fi
	if [ -n "$server_pid" ]; then kill -TERM "$server_pid" 2>/dev/null || true; fi
}
trap cleanup EXIT HUP INT TERM

flags=$(pkg-config --cflags x11 xft fontconfig x11-xcb xcb xcb-res)
libs=$(pkg-config --libs x11 xft fontconfig x11-xcb xcb xcb-res)
$cc -std=c99 -pedantic -Wall -Wextra -Wno-unused-function \
	-Wno-deprecated-declarations -DDWM_TEST -I"$root/src" \
	-D_DEFAULT_SOURCE -D_BSD_SOURCE -D_XOPEN_SOURCE=700L -DXINERAMA \
	-DVERSION=\"6.5\" $flags "$root/src/dwm.c" "$root/src/drw.c" \
	"$root/src/util.c" $libs -lm -o "$tmp/dwm"
$cc -std=c99 -pedantic -Wall -Wextra $flags "$root/src/tests/x11.c" \
	$libs -o "$tmp/x11"
$cc -std=c99 -pedantic -Wall -Wextra $flags "$root/src/transient.c" \
	$libs -o "$tmp/transient"

Xvfb "$display" -screen 0 800x600x24 -nolisten tcp -noreset >"$tmp/xvfb.log" 2>&1 &
server_pid=$!
export DISPLAY=$display
for i in 1 2 3 4 5 6 7 8 9 10; do
	if xdpyinfo >/dev/null 2>&1; then break; fi
	sleep 0.1
done
xdpyinfo >/dev/null 2>&1
printf 'dwm.borderpx: 7\n' | xrdb -merge

"$tmp/dwm" >"$tmp/dwm.log" 2>&1 &
dwm_pid=$!
sleep 0.2
"$tmp/transient" >"$tmp/transient.log" 2>&1 &
transient_pid=$!
"$tmp/x11"

printf '%s\n' 'X11 integration checks passed'
