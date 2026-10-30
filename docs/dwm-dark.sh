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
		msg="dwm-dark: $name failed (exit $status)"
		echo "$msg" >&2
		report=${report:+$report$'\n'}$msg
		failed=1
	}
}

step xfconf-query xfconf-query -c xsettings -p /Net/ThemeName -s "Adwaita-dark"
step gsettings gsettings set org.gnome.desktop.interface color-scheme 'prefer-dark'
step atc atc dark

step xrdb xrdb -merge <<'XRESOURCES'
dwm.color0: #282a36
dwm.color4: #ff79c6
dmenu.color0: #282a36
dmenu.color4: #ff79c6
dwm.borderpx: 1
dwm.selbordercolor: #b84f7b
XRESOURCES

step pkill pkill -USR2 -x dwm

if [ "$failed" -ne 0 ]; then
	notify-send -u critical "dwm theme" "$report" >/dev/null 2>&1 || :
else
	notify-send -u low "dwm theme" "Switched to dark theme" >/dev/null 2>&1 || :
fi

exit "$failed"
