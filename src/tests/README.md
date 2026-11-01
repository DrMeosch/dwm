# Regression checks

From the repository root, run `make test` with GCC or Clang, C99 support,
`pkg-config`, the configured X11, Xft, fontconfig, XCB, and Xinerama development
headers/libraries, and the system math library available. The test target
compiles temporary executables from the production sources. It covers layout
geometry and gap branches, core monitor/client state helpers (including
refusing scratchpad-only tag views, rejecting Xresources strings that exceed
their buffer, keeping the selected-tag font set disabled for an empty or NULL
`selfontweight`, parsing, measuring and click mapping of status2d codes with
every truncated, malformed or out-of-range form falling back to plain text,
plus palette loading that accepts only exactly `#rrggbb`, and finding the
dwmblocks PID without parsing anything when `pidof` fails or prints nothing,
and `buttonpress()` signalling the block under a status click, including a
status wider than the bar and the exact first and last pixel of each block, all
without X11, XTEST or `pidof`),
tag shifting,
UTF-8 decoding and drawing API guards, utility error behavior, and
build/configuration/manpage contracts, including the `selfontweight` selected-tag
font set, the PKGBUILD source and
dependencies and the LICENSE file shipped by `make dist`.

The state and geometry checks are headless and never execute configured desktop
commands. If Xvfb, `xdpyinfo`, `xrdb`, and `pkg-config` are available, `run.sh`
also starts a private X server and the production `dwm` event loop. When the
XTEST development files are available, an XTEST client checks that idle Mod+q
delivers a WM_DELETE_WINDOW request without further input, and that a click on
an unfocused client is replayed before subsequent typing; both deliveries must
occur without an external wake. If the files are unavailable, this input
regression check is reported as skipped. With the same files and `pidof`, and
no real dwmblocks running, a fake `dwmblocks` logs the signals it receives
while XTEST clicks the status of the bar: each click must reach the block under
the pointer, pixel-exact against the drawn status, including when the status
is wider than the bar; this is the pixel-level counterpart of the headless
`buttonpress()` check above, which runs everywhere. The production `transient.c`
executable is also exercised. An Xlib client verifies
resource-driven borders,
rendered bar text, a status that leaves only a narrow gap before it without
stale title pixels, colored status segments set through the root window name
(pixel colors, alignment against the right edge of the bar, that codes take
no width, and that text and rectangles pushed off the left edge by ^f advances
beyond 16-bit X11 coordinates are not drawn, and that the status background
still covers the bar), a `dwm.color<n>` palette change
followed by SIGUSR2 and a malformed palette value keeping its default,
MapRequest, tiled and floating ConfigureRequest handling, ConfigureNotify
delivery, and transient-window placement. Autostart is disabled
only in the `dwm` test build, so no configured desktop command is launched.

The Xlib client waits for the production `transient.c` window to be managed
before it creates its tiled window; this order is deliberate, so the tiled
window is always the newest, selected client for the bar checks.

The geometry harness supplies the monitor/client iteration and resize boundary
used by the layout code. It verifies requested rectangles; it does not test X
server placement, size hints, or full key event dispatch. The normal `make`
target remains the production build check.

The `sigusr2` check builds the production signal-pipe module and sends SIGUSR2
from a child process while the test process is blocked in `poll`. It verifies
that the signal wakes the wait, the reload callback observes updated resource
data, the callback runs outside the signal handler, and the pipe is drained.
This behavioral check does not need an X server.

The `borderwidth` check uses the Xresources parser to verify that
`dwm.borderpx` selects one or two pixels, ignores `dwm.color8` and dwmblocks
resources, and falls back to the configured width for missing, invalid, zero,
negative, and out-of-range values. The `selborder` check uses the Xresources
parser with dark and light resource databases. It verifies that
`dwm.selbordercolor` selects each theme color, `dwm.color8` cannot override it,
and the configured orange value is used when the dedicated resource is absent.

To capture the live reload visually, run:

```sh
DWM_X11_CAPTURE_DIR=/tmp/dwm-captures make test
```

The X11 integration test checks a one-pixel dark-theme border and a two-pixel
light-theme border on the same selected client before and after sending SIGUSR2
to the running test dwm. It also verifies a hidden client's tag assignment and
the selected tag view survive the reload. When capture is enabled, it writes
`dark.png` and `light.png` to that directory. CI
uploads both images as the `xresources-reload-screenshots` artifact and adds a
direct download link to the workflow summary.

The `eventloop` check runs the production `run()` with a MapRequest handler
that performs a raw XCB round trip (as `winpid()` does) while a second
MapRequest arrives, and verifies the loop dispatches the event XCB queued
instead of blocking in `poll`. It runs from `x11.sh`, so it needs the
private X server.

`x11.sh` also executes `docs/dwm-light.sh` and `docs/dwm-dark.sh` with stub
`xfconf-query`, `gsettings` and `atc`, a stub `pkill` that signals only the test
dwm, and a stub `notify-send` that records its arguments (so no real
notification is ever shown). With every desktop step failing, each script must
exit non-zero, name the three failed steps on stderr and send the same text as
a critical notification, whether `notify-send` succeeds or fails, yet the
selected border and the bar background must still switch to the light and dark
theme. After each run `xrdb -query` must show `dmenu.color0` and
`dmenu.color4` equal to `dwm.color0` and `dwm.color4`, so a dmenu started later
gets the theme colors. With every step succeeding, each script must exit zero,
stay silent and send the low-urgency "Switched to <theme> theme" notification.

The `sysmon` check covers the contract between dwm and the `dwmsysmon`
sampler without X: parsing the CPU line of `/proc/stat` (truncated, signed,
overflowing and otherwise malformed lines are no reading), the usage between two
readings (counters that stand still, go backwards or exceed the elapsed time are
no value), and the property payload (`cpu=<0-100>`, `temp=<0-150>` and
`fan=<rpm>[,<rpm>...]` with up to four fans of 0-30000 in at most 64 bytes;
oversized, empty and NUL-containing payloads are no value, and a malformed,
out-of-range or repeated field is missing while the others stay; every
combination of values survives encoding and decoding, and the decoder dwm had
before temperatures and fans still reads the CPU usage of a new sampler). It
also builds fake sysfs trees in a scratch directory for the sampler's sensor
sources: none at all, a Dell-like laptop (sparse `coretemp` inputs, the same two
fans on the fast `dell_ddv` and the slow `dell_smm` chip, a `dell_smm` label
that cannot be read, `nvme`, wifi, battery and `amdgpu` chips that must not be
used), two fans, one and none, labels that pick the package or die sensor
(`Package id`, `Tdie` over `Tctl`), the chip priority (`coretemp`, `k10temp`,
`zenpower`, `cpu_thermal`) and the `x86_pkg_temp` thermal zone fallback, two CPU
packages, unreadable and malformed or implausible readings (missing, and the
read reports the failure), more chips and fans than are used, attributes and
roots too long for the buffers, when sources are looked for again, and the
slower cadence for slow reads. `core.c` also checks the section's text with and
without each field, that its width follows the fields present but not their
values, and that it is centred and left out, never
overlapping the layout symbol or the status, on wide and narrow bars and at the
exact boundaries, that a held value expires on time and that dwm sets no poll
timeout without one, and that a click on the section or the gap up to the status
is neither a title, layout symbol nor status click, while a click on the title
left of it, on the status, or with the section off or left out for lack of room
keeps its usual meaning.

`sysmon-x11.c` runs from `x11.sh` against the running test dwm and recognises
the system monitor section by the bar pixels it produces. It enables the
section through Xresources and SIGUSR2, and uses itself as a stand-in sampler to
check that values show up and differ, that the temperature and every fan show
up and change the bar, that a malformed temperature or fan list is left out
without hiding the other fields, that garbage (wrong content, wrong type or
format, oversized, empty, removed) shows `-`, that a value nobody refreshes goes
stale by itself and stays while it is refreshed, and after each of these that
dwm still redraws a status change. The test dwm finds `dwmsysmon` through a
symbolic link first on its `PATH`, which the check points at a stub that exits
at once while it stands in for the sampler, and at the fixture build of the real
sampler otherwise; samplers are counted by their executable and `DISPLAY` in
`/proc`, so an installed `dwmsysmon` is never started or counted. It also checks that the section is centred
in the bar with a short and with a long status text, and is left out, without
overlapping anything, when a very long status text reaches the centre, with
the CPU usage alone and with the temperature and one, two or four fans. It first
checks that dwm, running with the section off by default, started no sampler.
It then shows the section so that dwm starts the real `dwmsysmon`, built with `SYSMON_STAT_PATH` pointing at a fixture
instead of `/proc/stat` and `SYSMON_SYS_PATH` at a fake sysfs tree instead of
`/sys`, and sampling at its fixed 2000 ms interval (with
`dwm.sysmonstale` raised to 4500), and checks the published value and the bar
for a changing load, a stopped (hung) sampler, a malformed and a missing stat file
(`-`, then recovery), a hidden section (the sampler exits and leaves no value,
and showing the section again starts a new one whose value comes back), a
sampler stopped with SIGSTOP while the section is hidden (dwm hides the section
and stays responsive, and when the section is shown again the sampler dwm
starts retires the stopped one once it continues), sensors appearing in the fake tree (the temperature
and both fans of a Dell-like laptop, then one fan, none, and a malformed
temperature, each published and drawn), a sensor read that hangs (a FIFO as the
temperature input: dwm stays responsive and lets the value go stale, and the
value returns once the read completes), a sampler started by hand taking over
from the one dwm started, and a clean exit on SIGTERM. With XTEST available it
also toggles the section with Super+Shift+b, which must stop the sampler and
remove its value, then start a new one whose value is shown. The test dwm is
left with the section hidden and no sampler running.

`core.c` also runs the command dwm hands to `system()` at startup against stub
`killall`, `dwmblocks` and `dwmsysmon` programs, with nothing else on `PATH`, and
checks that it starts `dwmblocks` but neither starts nor stops a sampler. With
the process start and the X property recorded instead, it checks that dwm starts
`dwmsysmon` exactly once each time the section turns on (at start-up with the
resource set, by Super+Shift+b or by a reload), never with the section off by
default or on a reload that changes nothing, and that hiding the section only
publishes the flag the sampler exits on.
`sysmon-lifecycle.sh` runs from `x11.sh` with two private X servers standing in
for two X sessions of one user, each with the test dwm, which starts a real
`dwmsysmon` (found first on its `PATH`) reading an advancing fixture and an
empty sysfs tree. Samplers are counted per display by their executable and
`DISPLAY` in `/proc`. With the section off by default no sampler runs on either
display; showing it through Xresources and SIGUSR2 starts exactly one sampler on
that display only. A replacement sampler on one display must retire that
display's sampler only. Hiding the section there must stop its sampler and
remove its value while the other display's sampler and value stay untouched, a
sampler started while the section is hidden must exit at once, and showing it
again must start a new one whose value comes back. A sampler stopped with
SIGSTOP before the section is hidden must be retired, once it continues, by the
one dwm starts when the section is shown again. Restarting dwm there must leave
the other session's sampler and value untouched; dwm started with the section
shown starts a sampler right away, and started with it hidden starts none. When
dwm exits normally with the X server still running, or is killed outright, its
sampler must exit and remove its value, and a sampler started after dwm is gone
must not linger.

SIGUSR1 stays reserved for dwmblocks; SIGUSR2 requests the Xresources reload.

`changelog.sh` feeds sample PR bodies to `.github/scripts/check-changelog.sh`, the
script behind the `PR changelog` workflow: a non-empty `## Changelog` section
(including `none`) passes; a missing, empty or comment-only one fails.

`changelog-feed.sh` runs `.github/scripts/extract-changelog.sh` and
`post-changelog.sh` (the `Changelog feed` workflow) against sample PR bodies and
a stub `gh`: merged PRs with changelog lines are commented on the open
"Changelog" issue (created and pinned when missing) with the date, PR link and
merge-commit link; `none`, empty sections and unmerged PRs post nothing.
