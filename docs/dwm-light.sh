#!/usr/bin/env bash
set -eu

xfconf-query -c xsettings -p /Net/ThemeName -s "Adwaita"
gsettings set org.gnome.desktop.interface color-scheme 'prefer-light'
atc light

xrdb -merge <<'XRESOURCES'
dwm.color0: #f8f8f2
dwm.color4: #555b6e
dwm.borderpx: 2
dwm.selbordercolor: #b84f7b
XRESOURCES

pkill -USR2 -x dwm
