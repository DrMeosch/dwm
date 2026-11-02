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

# Source the PKGBUILD without running any of its hooks and check its metadata.
bash -n "$root/src/PKGBUILD"
bash -c '
	. "$1"
	[ "$url" = https://github.com/drmeosch/dwm ]
	[ "${source[0]}" = git+https://github.com/drmeosch/dwm.git ]
	case " ${depends[*]} " in *" libxinerama "*) ;; *) exit 1 ;; esac
	[ "$install" = dwm.install ] && [ -f "$(dirname "$1")/$install" ]
	[ "$(type -t build)" = function ]
	[ "$(type -t package)" = function ]
' _ "$root/src/PKGBUILD"

# Run package() against a copy of the checkout with make stubbed out: it must
# find the Makefile and config.mk where the repository keeps them and install
# the license.
mkdir -p "$tmp/pkg/src/dwm" "$tmp/pkg/pkg"
cp "$root/Makefile" "$root/LICENSE" "$tmp/pkg/src/dwm/"
mkdir "$tmp/pkg/src/dwm/src"
cp "$root/src/config.mk" "$tmp/pkg/src/dwm/src/"
bash -c '
	. "$1"
	srcdir=$2/src pkgdir=$2/pkg
	make() { [ -f Makefile ] && [ -f src/config.mk ] || exit 1; }
	cd "$srcdir"
	package
' _ "$root/src/PKGBUILD" "$tmp/pkg"
[ -s "$tmp/pkg/pkg/usr/share/licenses/dwm-drmeosch-git/LICENSE" ]

# make dist, run in a scratch copy, must ship the license and leave the repo alone.
mkdir -p "$tmp/dist/src"
cp "$root/Makefile" "$root/LICENSE" "$tmp/dist/"
cp -R "$root"/src/. "$tmp/dist/src/"
(cd "$tmp/dist" && make dist >/dev/null)
tar -tzf "$tmp/dist/dwm-$version.tar.gz" | grep -qx "dwm-$version/LICENSE"
tar -tzf "$tmp/dist/dwm-$version.tar.gz" | grep -qx "dwm-$version/src/config.mk"
# the sampler and its shared module must ship with dwm, and install must install the sampler
for f in sysmon.c sysmon.h sensors.c sensors.h dwmsysmon.c dwm.install; do
	tar -tzf "$tmp/dist/dwm-$version.tar.gz" | grep -qx "dwm-$version/src/$f"
done
(cd "$tmp/dist" && make -n install DESTDIR="$tmp/inst" | grep -q 'install -Dm755 \./dwm \./dwmsysmon ')
[ ! -e "$tmp/dist/dwm-$version" ]
[ ! -e "$root/dwm-$version.tar.gz" ]
[ -s "$root/LICENSE" ]

# make install gives the sampler CAP_PERFMON, which the Intel GPU usage takes, and says
# how to give it when that fails; the Arch package does the same after installing or
# upgrading, which drops it. setcap is a stub that logs its arguments or fails.
mkdir -p "$tmp/inst/src" "$tmp/inst/stub"
cp "$root/Makefile" "$tmp/inst/"
cp "$root/src/config.mk" "$root/src/dwm.1" "$tmp/inst/src/"
: > "$tmp/inst/dwm"
: > "$tmp/inst/dwmsysmon"
printf '#!/bin/sh\necho "$*" >> "%s"\n' "$tmp/inst/setcap.log" > "$tmp/inst/stub/setcap"
chmod +x "$tmp/inst/stub/setcap"
(cd "$tmp/inst" && PATH="$tmp/inst/stub:$PATH" make -o all install DESTDIR="$tmp/inst/root" PREFIX=/usr \
	>/dev/null 2>"$tmp/inst/err")
[ "$(cat "$tmp/inst/setcap.log")" = "cap_perfmon=p $tmp/inst/root/usr/bin/dwmsysmon" ]
[ ! -s "$tmp/inst/err" ]
PATH="$tmp/inst/stub:$PATH" bash -c '. "$1"; post_install; post_upgrade' _ "$root/src/dwm.install" > "$tmp/inst/out"
[ ! -s "$tmp/inst/out" ]
[ "$(tail -n 2 "$tmp/inst/setcap.log")" = "cap_perfmon=p /usr/bin/dwmsysmon
cap_perfmon=p /usr/bin/dwmsysmon" ]
printf '#!/bin/sh\nexit 1\n' > "$tmp/inst/stub/setcap"
(cd "$tmp/inst" && PATH="$tmp/inst/stub:$PATH" make -o all install DESTDIR="$tmp/inst/root" PREFIX=/usr \
	>/dev/null 2>"$tmp/inst/err")
grep -qx 'dwmsysmon: to show the Intel GPU usage, run as root: setcap cap_perfmon=p /usr/bin/dwmsysmon' "$tmp/inst/err"
PATH="$tmp/inst/stub:$PATH" bash -c '. "$1"; post_install && post_upgrade' _ "$root/src/dwm.install" > "$tmp/inst/out"
[ "$(grep -cx 'dwmsysmon: to show the Intel GPU usage, run as root: setcap cap_perfmon=p /usr/bin/dwmsysmon' "$tmp/inst/out")" = 2 ]

# The selected tag is drawn with a heavier font set built from the configured fonts.
grep -q '^static const char \*selfontweight' "$root/src/config.h"
grep -q 'drw_setfontset(drw, selfonts)' "$root/src/dwm.c"
grep -q ':weight=%s' "$root/src/dwm.c"

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
	"$root/src/dwm.c" "$root/src/drw.c" "$root/src/borderwidth.c" \
	"$root/src/selborder.c" \
	"$root/src/sigusr2.c" "$root/src/sysmon.c" "$root/src/util.c" \
	$(pkg-config --libs x11 x11-xcb xinerama xft fontconfig xcb xcb-res 2>/dev/null || true) \
	-lm -o "$tmp/dwm"
if "$tmp/dwm" -v >"$tmp/version.out" 2>&1; then
	exit 1
fi
grep -q "dwm-$version" "$tmp/version.out"

printf '%s\n' 'build and documentation contract checks passed'
