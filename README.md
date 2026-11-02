# dwm (DrMeosch's build)

DrMeosch's build of [dwm](https://dwm.suckless.org/) 6.5, based on [Luke Smith's build](https://github.com/LukeSmithxyz/dwm), with Xresources-driven theming and live dark/light reload. Security fixes from versions 6.6-6.8 are backported.

## Features

- Luke's build patches: clickable status bar (statuscmd), scratchpad, bstack/fibonacci/deck/centered-master layouts, vanitygaps, sticky windows, hide-vacant-tags, stacker, shiftview, swallow, true fullscreen. See [`src/README.md`](src/README.md) and [`src/config.h`](src/config.h) for bindings.
- Colored status bar segments ([status2d](https://dwm.suckless.org/patches/status2d/), reimplemented with bounds-checked parsing): `^c#rrggbb^`/`^b#rrggbb^` truecolor codes and `^C<0-15>^`/`^B<0-15>^` codes that follow `dwm.color0`..`dwm.color15`. Clickable status regions (statuscmd) stay aligned. Grammar in [`src/dwm.1`](src/dwm.1).
- Optional system monitor section centred in the bar (CPU usage with a small graph of its recent samples, plus the CPU temperature and every fan's RPM where the machine reports them, and the GPU usage with its own graph on AMD and Intel GPUs, read without ever waking a sleeping GPU (Intel's needs a capability, see [Intel GPU usage](#intel-gpu-usage)); when a long status text leaves it little room the graphs shrink first, and the section is left out when there is none), off by default: <kbd>super+shift+b</kbd> or `dwm.showsysmon: 1`. A separate sampler, `dwmsysmon`, runs only while the section is shown: dwm starts it when the section is turned on and it exits when the section is turned off. It reads the sensors and publishes the values in a root window property, so dwm only draws them and stays responsive when the sampler or a sensor is missing, slow, stuck or sends garbage (the CPU then shows `-` with an empty graph, and a missing temperature or fan is not shown). See [`src/dwm.1`](src/dwm.1).
- Xresources colors and settings (`dwm.color0`, `dwm.color4`, `dwm.borderpx`, `dwm.selbordercolor`, and `dwm.color1`..`dwm.color15` for status colors).
- Live theme reload: `SIGUSR2` makes dwm re-read Xresources (`src/sigusr2.c`). `SIGUSR1` stays reserved for dwmblocks.
- Configurable border width (1 or 2 px) and selected-border color (`src/borderwidth.c`, `src/selborder.c`).

## Layout

| Path | Purpose |
| --- | --- |
| `src/` | dwm sources, `config.h`, `config.mk`, man page, `PKGBUILD` |
| `src/tests/` | Regression checks (see [`src/tests/README.md`](src/tests/README.md)) |
| `docs/dwm-dark.sh`, `docs/dwm-light.sh` | Switch GTK/xfconf theme, merge Xresources (`dwm.*` plus `dmenu.color0`/`dmenu.color4`), then `pkill -USR2 -x dwm` |
| `src/dwmsysmon.c`, `src/sensors.c`, `src/sysmon.c` | System monitor sampler, its temperature, fan and GPU sources, and the shared value format |
| `Makefile` | Top-level build, test, install, and dist targets |
| `treehouse.toml` | Worktree pool config |
| `.github/workflows/ci.yml` | CI: `make test`, then `make` |

## Build and install

Requires a C99 compiler, `pkg-config`, and the X11, Xft, fontconfig, XCB, and Xinerama development headers.

```sh
make                 # build ./dwm
sudo make install    # install to $(PREFIX) (default /usr/local)
make test            # regression checks
```

On Arch, `src/PKGBUILD` can be used with `makepkg -si`.

### Intel GPU usage

An Intel GPU has no busy percentage in sysfs. Its real usage, the share of
the time its busiest engine is busy (as `intel_gpu_top` and `btop` show it),
comes from the engine busy counters of the i915 perf PMU, and opening those
takes the `CAP_PERFMON` capability. `make install`, and the Arch package
after every install or upgrade, give it to `dwmsysmon` only
(`setcap cap_perfmon=p`: permitted only, so a system that does not allow it
still runs the sampler; it makes the capability effective to open the counters
and then drops it); dwm itself stays unprivileged and only draws.
Where that fails, it prints the one step left, as root:

```sh
setcap cap_perfmon=p /usr/local/bin/dwmsysmon
```

Without it the Intel GPU is not shown; AMD GPUs need nothing.

`dwmsysmon` uses the capability only to open the counters: it does so first
thing, before it reads its arguments or connects to X, then gives up every
capability for good (it clears the effective, permitted, inheritable and
ambient sets and sets `no_new_privs`) and reads the counters through the
descriptors it keeps open. So whatever it later parses, the X server's
replies or sysfs files, runs without any privilege, and an Intel GPU that
appears later is only shown after the sampler is started again (as toggling
the section does). Opening the counters, even of a suspended GPU, and keeping
them open do not wake it or keep it awake: the kernel counts engine busy time
in software and samples only engines that are already awake.

Source: <https://github.com/DrMeosch/dwm>.

## Theme switching

```sh
docs/dwm-dark.sh     # dark theme
docs/dwm-light.sh    # light theme
```

Both scripts expect `xfconf-query`, `gsettings`, `atc`, `xrdb`, and a running dwm. The desktop steps (`xfconf-query`, `gsettings`, `atc`) are best-effort: a failing one is reported on stderr by name and the Xresources merge and `SIGUSR2` reload still run, so one failing desktop step cannot leave dwm on the old colors. The script then exits non-zero. It also sends the result with `notify-send` (optional, best-effort): a critical notification with the same failed-step text, or a low-urgency "Switched to dark/light theme" when everything worked. The merge also sets `dmenu.color0` and `dmenu.color4` to the theme's `dwm.color0` and `dwm.color4`; dmenu builds that read Xresources at launch (such as the LARBS one) pick the theme up the next time they start.

## Testing

`make test` runs headless state and geometry checks, plus an Xvfb-based integration run when Xvfb, `xdpyinfo`, and `xrdb` are present. Set `DWM_X11_CAPTURE_DIR=/tmp/dwm-captures` to save dark/light reload screenshots.

## Contributing

Every pull request needs a `## Changelog` section in its description (the template provides it) with one or two user-facing lines starting with Added, Changed, Fixed, Removed or Security, or `none` for internal-only changes. The `PR changelog` workflow fails without it; PRs from renovate, dependabot and github-actions are exempt. There is no `CHANGELOG.md` to edit. After a PR is merged, the `Changelog feed` workflow appends its Changelog lines (with the PR and merge-commit links) as a comment on the pinned "Changelog" issue; `none` is skipped.

## License and credits

Released under the MIT/X Consortium license; see [`LICENSE`](LICENSE) for the full list of copyright holders.

- This is a fork of [Luke Smith's build of dwm](https://github.com/LukeSmithxyz/dwm), which supplies the patch set and key bindings described in [`src/README.md`](src/README.md).
- [dwm](https://dwm.suckless.org/) is by the [suckless](https://suckless.org/) project.
- Patches used: [statuscmd](https://dwm.suckless.org/patches/statuscmd/), [status2d](https://dwm.suckless.org/patches/status2d/) (reimplemented), [xresources](https://dwm.suckless.org/patches/xresources/), [hide_vacant_tags](https://dwm.suckless.org/patches/hide_vacant_tags/), [stacker](https://dwm.suckless.org/patches/stacker/), [shiftview](https://dwm.suckless.org/patches/nextprev/), [vanitygaps](https://dwm.suckless.org/patches/vanitygaps/), [swallow](https://dwm.suckless.org/patches/swallow/), plus scratchpad, sticky, the bstack/fibonacci/deck/centered-master layouts and true fullscreen.
