#!/bin/sh
set -eu

root=$1
cc=${CC:-cc}
test_cflags=${TEST_CFLAGS:-'-std=c99 -pedantic -Wall -Wextra -Wno-unused-function -Wno-deprecated-declarations'}
test_libs=${TEST_LIBS:-}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

compile_and_run() {
	name=$1
	shift
	$cc $test_cflags "$@" -lm -o "$tmp/$name"
	"$tmp/$name"
}

compile_and_run righttile "$root/src/tests/righttile.c"
compile_and_run layouts "$root/src/tests/layouts.c"
compile_and_run shiftview "$root/src/tests/shiftview.c"
compile_and_run utilities "$root/src/tests/utilities.c"
compile_and_run sigusr2 "$root/src/tests/sigusr2.c" "$root/src/sigusr2.c"

$cc $test_cflags "$root/src/tests/selborder.c" "$root/src/selborder.c" \
	$test_libs -lX11 -lm -o "$tmp/selborder"
"$tmp/selborder"

$cc $test_cflags "$root/src/tests/core.c" "$root/src/drw.c" "$root/src/sigusr2.c" \
	"$root/src/selborder.c" "$root/src/util.c" \
	$test_libs -lm -o "$tmp/core"
"$tmp/core"

$cc $test_cflags "$root/src/tests/drawing.c" "$root/src/util.c" \
	$test_libs -lm -o "$tmp/drawing"
"$tmp/drawing"

CC="$cc" TEST_CFLAGS="$test_cflags" TEST_LIBS="$test_libs" \
	sh "$root/src/tests/contracts.sh" "$root"

sh "$root/src/tests/x11.sh" "$root"
