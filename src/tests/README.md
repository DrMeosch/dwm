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
used by the layout code. The normal `make` target remains the production build
check.
