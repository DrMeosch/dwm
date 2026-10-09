#!/bin/sh
set -eu

root=$1
cc=${CC:-cc}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

version=$(sed -n 's/^VERSION = //p' "$root/src/config.mk")
[ -n "$version" ]

# The installed man page must receive the configured version and no template token.
sed "s/VERSION/$version/g" "$root/src/dwm.1" > "$tmp/dwm.1"
! grep -q 'VERSION' "$tmp/dwm.1"
grep -Fq ".TH DWM 1 dwm\\-$version" "$tmp/dwm.1"
grep -Fq ".B \\-v" "$tmp/dwm.1"

# Keep package metadata parseable without running package hooks or network commands.
bash -n "$root/src/PKGBUILD"
grep -q '^pkgname=' "$root/src/PKGBUILD"
grep -q '^build()' "$root/src/PKGBUILD"
grep -q '^package()' "$root/src/PKGBUILD"

# Compile the X helper without opening a display; execution is intentionally excluded.
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists x11; then
	$cc -std=c99 -Wall -Wextra -Wno-unused-parameter \
		$(pkg-config --cflags x11) -fsyntax-only "$root/src/transient.c"
fi

# Exercise the production command-line version path without launching dwm or X.
$cc -std=c99 -pedantic -Wall -Wno-unused-function -Wno-deprecated-declarations \
	-I"$root/src" -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_XOPEN_SOURCE=700L \
	-DXINERAMA -DVERSION=\"$version\" \
	$(pkg-config --cflags fontconfig xft x11 x11-xcb xinerama xcb xcb-res 2>/dev/null || true) \
	"$root/src/dwm.c" "$root/src/drw.c" "$root/src/util.c" \
	$(pkg-config --libs x11 x11-xcb xinerama xft fontconfig xcb xcb-res 2>/dev/null || true) \
	-lm -o "$tmp/dwm"
if "$tmp/dwm" -v >"$tmp/version.out" 2>&1; then
	exit 1
fi
grep -q "dwm-$version" "$tmp/version.out"

printf '%s\n' 'build and documentation contract checks passed'
