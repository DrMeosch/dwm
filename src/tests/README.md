# Regression checks

From the repository root, run `make test` with GCC or Clang, C99 support,
`pkg-config`, the configured X11, Xft, fontconfig, XCB, and Xinerama development
headers/libraries, and the system math library available. The test target
compiles temporary executables from the production sources. It covers layout
geometry and gap branches, core monitor/client state helpers, tag shifting,
UTF-8 decoding and drawing API guards, utility error behavior, and
build/configuration/manpage contracts.

The state and geometry checks are headless and never execute configured desktop
commands. If Xvfb, `xdpyinfo`, `xrdb`, and `pkg-config` are available, `run.sh`
also starts a private X server and the production `dwm` event loop. When the
XTEST development files are available, an XTEST client checks that idle Mod+q
delivers a WM_DELETE_WINDOW request without further input, and that a click on
an unfocused client is replayed before subsequent typing; both deliveries must
occur without an external wake. If the files are unavailable, this input
regression check is reported as skipped. The production `transient.c`
executable is also exercised. An Xlib client verifies
resource-driven borders,
rendered bar text, MapRequest, tiled and floating ConfigureRequest handling,
ConfigureNotify delivery, and transient-window placement. Autostart is disabled
only in the `dwm` test build, so no configured desktop command is launched.

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
direct download link to the workflow summary. SIGUSR1 stays reserved for
dwmblocks; SIGUSR2 requests the Xresources reload.
