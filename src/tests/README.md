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
theme. After each run `xrdb -query` must show `dmenu.color0` and `dmenu.color4` equal to
`dwm.color0` and `dwm.color4`, so a dmenu started later gets the theme colors.
With every step succeeding, each script must exit zero, stay silent and
send the low-urgency "Switched to <theme> theme" notification.

SIGUSR1 stays reserved for dwmblocks; SIGUSR2 requests the Xresources reload.

`changelog.sh` feeds sample PR bodies to `.github/scripts/check-changelog.sh`, the
script behind the `PR changelog` workflow: a non-empty `## Changelog` section
(including `none`) passes; a missing, empty or comment-only one fails.

`changelog-feed.sh` runs `.github/scripts/extract-changelog.sh` and
`post-changelog.sh` (the `Changelog feed` workflow) against sample PR bodies and
a stub `gh`: merged PRs with changelog lines are commented on the open
"Changelog" issue (created and pinned when missing) with the date, PR link and
merge-commit link; `none`, empty sections and unmerged PRs post nothing.
