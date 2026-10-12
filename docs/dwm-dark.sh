#!/usr/bin/env bash
set -eu

xfconf-query -c xsettings -p /Net/ThemeName -s "Adwaita-dark"
gsettings set org.gnome.desktop.interface color-scheme 'prefer-dark'
atc dark

xrdb -merge <<'XRESOURCES'
dwm.color0: #282a36
dwm.color4: #ff79c6
dwm.borderpx: 1
dwm.selbordercolor: #b84f7b
XRESOURCES

pkill -USR2 -x dwm
