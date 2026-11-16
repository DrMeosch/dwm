# AGENTS.md

Custom dwm 6.5 build (C, X11) with backported security fixes from 6.6_6.8.
See [README.md](README.md) for features and layout.

## Commands

- Build: `make` (produces `./dwm`).
- Test: `make test` (also what CI runs, followed by `make`). Run both before finishing a change.
- In fresh environments, ensure `jq`, `pkg-config` and the configured X11 development libraries are installed. For full X11 coverage also install Xvfb, `xdpyinfo`, `xrdb`, `xprop` and XTEST development files; check that relevant X11 and input tests did not skip.
- Clean: `make clean`.
- When compiling `src/tests/run.sh` tests outside `make test`, reuse the Makefile's `TEST_CFLAGS` and `TEST_LIBS`, including feature macros and include paths.

## Conventions

- Source lives in `src/`; the top-level `Makefile` includes `src/config.mk`.
- Follow suckless style: tabs, C99, `-pedantic -Wall -Wextra` clean, minimal abstraction.
- Configuration is compile-time in `src/config.h`; runtime theming goes through Xresources (`dwm.*`) and `SIGUSR2` reload.
- Never use `SIGUSR1` for dwm features; it is reserved for dwmblocks.
- A new `.c` file must be added to `SRC` in the `Makefile`, to the `dist` target, and to the dependency lines if it has a header.
- Add or update a regression check in `src/tests/` for behavior changes, and keep `src/tests/README.md` accurate.

## Gotchas

- `.envrc*` files are gitignored and hold credentials. Never read them into output, commit them, or copy their values into docs or code.
- Never print credential stores such as `~/.docker/config.json` or private keys; check for existence or use the consuming tool instead.
- `make dist` keeps the `src/` layout and ships the top-level `Makefile`; a new source file needs adding to its `cp` list (`config.h` includes `.c` files directly).
- `docs/dwm-*.sh` call desktop tools (`xfconf-query`, `gsettings`, `atc`) and `pkill -USR2 -x dwm`; do not run them in a shared or CI environment.
- When testing theme scripts, stub desktop tools and `notify-send`, and ensure the `pkill` stub can signal only the test dwm.
- Tests must never launch configured autostart or desktop commands.
- Keep throwaway prototypes, review logs and helper scripts outside the repository tree, e.g. under `/tmp`, unless requested as deliverables.
