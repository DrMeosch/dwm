# Regression checks

From the repository root, run `make test` with GCC or Clang, C99 support,
`pkg-config`, the configured X11, Xft, fontconfig, XCB, and Xinerama development
headers/libraries, and the system math library available. The test target
compiles temporary executables from the production sources. It covers layout
geometry and gap branches, core monitor/client state helpers, tag shifting,
UTF-8 decoding and drawing API guards, utility error behavior, and
build/configuration/manpage contracts.

The state and geometry checks are headless and never execute configured desktop
commands. If Xvfb, `xdpyinfo`, and `xrdb` are all available, `run.sh` also starts
a private X server, the production `dwm` event loop, and the production
`transient.c` executable. An Xlib client verifies resource-driven borders,
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

The `selborder` check uses the Xresources parser with dark and light resource
databases. It verifies that `dwm.selbordercolor` selects each theme color,
`dwm.color8` cannot override it, and the configured orange value is used when
the dedicated resource is absent.

To capture the live reload visually, run:

```sh
DWM_X11_CAPTURE_DIR=/tmp/dwm-captures make test
```

The X11 integration test checks the same selected client's focus, one-pixel
border width, and rendered border color before and after sending SIGUSR2 to the
running test dwm, then writes `dark.png` and `light.png` to that directory. CI
uploads both images as the `xresources-reload-screenshots` artifact and adds a
direct download link to the workflow summary. SIGUSR1 stays reserved for
dwmblocks; SIGUSR2 requests the Xresources reload.
