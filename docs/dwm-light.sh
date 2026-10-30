#!/usr/bin/env bash
set -u

# Desktop steps are best-effort and the dwm reload always runs, so one missing
# tool or theme file cannot leave dwm on the old colors. Exit non-zero at the
# end if any step failed. The result is also sent with notify-send, which is
# best-effort too.
failed=0
report=
step() {
	local name=$1 status msg
	shift
	"$@" || {
		status=$?
		msg="dwm-light: $name failed (exit $status)"
		echo "$msg" >&2
		report=${report:+$report$'\n'}$msg
		failed=1
	}
}

step xfconf-query xfconf-query -c xsettings -p /Net/ThemeName -s "Adwaita"
step gsettings gsettings set org.gnome.desktop.interface color-scheme 'prefer-light'
step atc atc light

step xrdb xrdb -merge <<'XRESOURCES'
dwm.color0: #f8f8f2
dwm.color4: #555b6e
dmenu.color0: #f8f8f2
dmenu.color4: #555b6e
dwm.borderpx: 2
dwm.selbordercolor: #b84f7b
XRESOURCES

step pkill pkill -USR2 -x dwm

if [ "$failed" -ne 0 ]; then
	notify-send -u critical "dwm theme" "$report" >/dev/null 2>&1 || :
else
	notify-send -u low "dwm theme" "Switched to light theme" >/dev/null 2>&1 || :
fi

exit "$failed"
