# DrMeosch's build of dwm

Based on [Luke Smith's build](https://github.com/LukeSmithxyz/dwm) of [dwm](https://dwm.suckless.org/). The patch set below comes from that build unless noted.

## FAQ

> What are the bindings?

This is suckless, mmmbud, the source code is the documentation! Check out [config.h](config.h).

## Patches and features

- [Clickable statusbar](https://dwm.suckless.org/patches/statuscmd/) with [Luke Smith's build of dwmblocks](https://github.com/lukesmithxyz/dwmblocks).
- [status2d](https://dwm.suckless.org/patches/status2d/): colored status text via `^c#rrggbb^`, `^b#rrggbb^`, `^C<0-15>^` (`dwm.color<n>`), `^d^`, `^r` and `^f`. It is not the upstream patch: the parser is bounds-checked, malformed codes show as plain text, and one walker measures and draws so the clickable statusbar regions stay correct. See `dwm.1`.
- Reads [xresources](https://dwm.suckless.org/patches/xresources/) colors/variables (i.e. works with `pywal`, etc.).
- scratchpad: Accessible with <kbd>mod+shift+enter</kbd>.
- New layouts: bstack, fibonacci, deck, centered master and more. All bound to keys <kbd>super+(shift+)t/y/u/i</kbd>.
  Every tag starts on the right-side master layout (righttile); see `defaultlayout` in `config.h`.
- True fullscreen (<kbd>super+f</kbd>) and prevents focus shifting.
- Windows can be made sticky (<kbd>super+s</kbd>).
- [hide vacant tags](https://dwm.suckless.org/patches/hide_vacant_tags/) hides tags with no windows.
- [stacker](https://dwm.suckless.org/patches/stacker/): Move windows up the stack manually (<kbd>super-K/J</kbd>).
- [shiftview](https://dwm.suckless.org/patches/nextprev/): Cycle through tags (<kbd>super+g/;</kbd>).
- [vanitygaps](https://dwm.suckless.org/patches/vanitygaps/): Gaps allowed across all layouts.
- [swallow patch](https://dwm.suckless.org/patches/swallow/): if a program run from a terminal would make it inoperable, it temporarily takes its place to save space.


## Installation for newbs

```bash
git clone https://github.com/DrMeosch/dwm.git
cd dwm
sudo make install
```

There is also a `PKGBUILD` usable on distributions with pacman. Run `makepkg -si` instead of `sudo make install`.

## License and credits

MIT/X Consortium license, see `../LICENSE`. Built on [Luke Smith's dwm build](https://github.com/LukeSmithxyz/dwm) and [suckless dwm](https://dwm.suckless.org/), with the patches linked above.
