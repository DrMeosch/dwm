# dwm (custom build)

A customized build of [dwm](https://dwm.suckless.org/) 6.5, derived from Luke Smith's build, with Xresources-driven theming and live dark/light reload. Security fixes from versions 6.6-6.8 were backported by gpt-6 models.

## Features

- Luke's build patches: clickable status bar (statuscmd), scratchpad, bstack/fibonacci/deck/centered-master layouts, vanitygaps, sticky windows, hide-vacant-tags, stacker, shiftview, swallow, true fullscreen. See [`src/README.md`](src/README.md) and [`src/config.h`](src/config.h) for bindings.
- Colored status bar segments ([status2d](https://dwm.suckless.org/patches/status2d/), reimplemented with bounds-checked parsing): `^c#rrggbb^`/`^b#rrggbb^` truecolor codes and `^C<0-15>^`/`^B<0-15>^` codes that follow `dwm.color0`..`dwm.color15`. Clickable status regions (statuscmd) stay aligned. Grammar in [`src/dwm.1`](src/dwm.1).
- Xresources colors and settings (`dwm.color0`, `dwm.color4`, `dwm.borderpx`, `dwm.selbordercolor`, and `dwm.color1`..`dwm.color15` for status colors).
- Live theme reload: `SIGUSR2` makes dwm re-read Xresources (`src/sigusr2.c`). `SIGUSR1` stays reserved for dwmblocks.
- Configurable border width (1 or 2 px) and selected-border color (`src/borderwidth.c`, `src/selborder.c`).

## Layout

| Path | Purpose |
| --- | --- |
| `src/` | dwm sources, `config.h`, `config.mk`, man page, `PKGBUILD` |
| `src/tests/` | Regression checks (see [`src/tests/README.md`](src/tests/README.md)) |
| `docs/dwm-dark.sh`, `docs/dwm-light.sh` | Switch GTK/xfconf theme, merge Xresources, then `pkill -USR2 -x dwm` |
| `Makefile` | Top-level build, test, install, and devcontainer targets |
| `.devcontainer/`, `treehouse.toml` | Development container and worktree pool config |
| `.github/workflows/ci.yml` | CI: `make test`, then `make` |

## Build and install

Requires a C99 compiler, `pkg-config`, and the X11, Xft, fontconfig, XCB, and Xinerama development headers.

```sh
make                 # build ./dwm
sudo make install    # install to $(PREFIX) (default /usr/local)
make test            # regression checks
```

On Arch, `src/PKGBUILD` can be used with `makepkg -si`.

## Theme switching

```sh
docs/dwm-dark.sh     # dark theme
docs/dwm-light.sh    # light theme
```

Both scripts expect `xfconf-query`, `gsettings`, `atc`, `xrdb`, and a running dwm. The desktop steps (`xfconf-query`, `gsettings`, `atc`) are best-effort: a failing one is reported on stderr by name and the Xresources merge and `SIGUSR2` reload still run, so one failing desktop step cannot leave dwm on the old colors. The script then exits non-zero. It also sends the result with `notify-send` (optional, best-effort): a critical notification with the same failed-step text, or a low-urgency "Switched to dark/light theme" when everything worked.

## Testing

`make test` runs headless state and geometry checks, plus an Xvfb-based integration run when Xvfb, `xdpyinfo`, and `xrdb` are present. Set `DWM_X11_CAPTURE_DIR=/tmp/dwm-captures` to save dark/light reload screenshots.
