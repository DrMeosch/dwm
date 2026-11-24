# Reproduced defects

Production code is unchanged. All assertions below remain enabled and fail
normally, without expected-failure masks. Severity is assessed by user impact:
Medium means incorrect window-manager behavior with a recoverable workaround.
These findings were reproduced against the current production sources in a
`DWM_TEST` build, which disables desktop autostart only.

## Reproduction environment

Run from the repository root with the build dependencies in `src/config.mk`,
`jq`, `pkg-config`, Xvfb, `xdpyinfo` and `xrdb` installed. XTEST development
files were available during validation, but these seven state fixtures use Xlib
and do not require physical input or XTEST.

Each command below compiles temporary test binaries, starts its own 1200x800
Xvfb and dwm, disables terminal launches with a stub, runs the selected fixture,
and cleans up. It never signals an installed dwm or launches desktop commands.
An exit status of 1 is the genuine assertion failure, not an expected pass.
The selectors allow reproduction even when the earlier switcher test blocks
`make test`.

## 1. Fullscreen transitions erase unrelated window states

- **Severity:** Medium. An application or desktop component loses its declared
  sticky/attention or other window states merely by entering or leaving fullscreen.
- **Affected production file:** `src/dwm.c`, `setfullscreen()`.
- **Failing test:** `src/tests/x11.c`,
  `check_fullscreen_state_preservation()`; **24 failing assertions**.
- **Reproduce:**
  ```sh
  DWM_X11_STATE_FIXTURE=fullscreen-state-preservation sh src/tests/x11.sh "$PWD"
  ```
  Map a tiled or dialog-style floating window with one or two unrelated atoms
  in its 32-bit ATOM `_NET_WM_STATE` property. Send ADD or TOGGLE for
  `_NET_WM_STATE_FULLSCREEN`, with fullscreen in either message slot. Before
  testing REMOVE or TOGGLE off, reinstall the full list including fullscreen
  so the exit check is independent of the entry failure.
- **Expected:** Add fullscreen to the existing atom set on entry; remove only
  fullscreen on exit. Preserve all other atoms, with no duplicate fullscreen.
- **Actual:** Entry replaces the property with fullscreen alone; exit replaces
  it with an empty ATOM list. For example, tiled ADD with two unrelated states
  reports `got 1 atoms, expected 3`; REMOVE reports `got 0 atoms, expected 2`.
- **Coverage and controls:** Both client types, zero/one/two unrelated states,
  ADD/REMOVE/TOGGLE, both message slots, repeated requests and unrelated
  requests. Empty-state controls, repeated and unrelated no-op requests,
  fullscreen geometry, border restoration and focus pass. This test does not
  require dwm to implement sticky or attention behavior, only to preserve the
  unrelated atoms when modifying fullscreen.
- **Cause:** Both branches use `XChangeProperty(..., PropModeReplace, ...)`
  without retaining the other atoms.

## 2. Initial fullscreen is ignored when it is not the first state atom

- **Severity:** Medium. Applications requesting fullscreen before mapping are
  displayed as ordinary windows depending on the order of their state atoms.
- **Affected production file:** `src/dwm.c`, `getatomprop()` and
  `updatewindowtype()`.
- **Failing test:** `src/tests/x11.c`, `check_fullscreen_properties()`;
  **6 initial-geometry assertions** plus **3 consequent floating-resize
  assertions**, within the fixture's 15 failures.
- **Reproduce:**
  ```sh
  DWM_X11_STATE_FIXTURE=fullscreen-properties sh src/tests/x11.sh "$PWD"
  ```
  Map either client type with a 32-bit ATOM `_NET_WM_STATE` containing
  `[unrelated, fullscreen]`, `[unrelated, fullscreen, unrelated]`, or ten
  atoms with fullscreen last.
- **Expected:** Recognize fullscreen anywhere in the list, cover the entire
  1200x800 screen, remove the border and remain fullscreen on resize requests.
- **Actual:** The tiled client remains at `(416,33) 751x755 border 1`; the
  floating client remains at `(449,299) 300x200 border 1` and accepts a resize.
- **Cause:** `getatomprop()` returns only the first atom, and
  `updatewindowtype()` compares that atom rather than scanning the list.
  Absent, empty, unrelated, wrong-type and wrong-format controls pass.

## 3. Applications can resize recognized fullscreen windows

- **Severity:** Medium. A fullscreen window can shrink and expose the desktop
  while still claiming to be fullscreen and retaining its zero border.
- **Affected production file:** `src/dwm.c`, `configurerequest()`.
- **Failing test:** `src/tests/x11.c`, `check_fullscreen_properties()`;
  **6 resize assertions**, within the fixture's 15 failures.
- **Reproduce:**
  ```sh
  DWM_X11_STATE_FIXTURE=fullscreen-properties sh src/tests/x11.sh "$PWD"
  ```
  Map either client type with fullscreen alone, first, or duplicated in its
  initial state list, then request a 137x119 size through `XResizeWindow()`.
- **Expected:** Keep the fullscreen 1200x800 geometry and zero border.
- **Actual:** Geometry changes from `(0,0) 1200x800 border 0` to
  `(0,0) 137x119 border 0` for all six recognized-fullscreen cases.
- **Cause:** Fullscreen sets `isfloating`, and `configurerequest()` processes
  the request as an ordinary floating resize without a fullscreen guard.

## 4. Invalid fullscreen actions exit fullscreen

- **Severity:** Medium. A malformed client message unexpectedly changes a
  managed window's valid fullscreen state and geometry.
- **Affected production file:** `src/dwm.c`, `clientmessage()`.
- **Failing test:** `src/tests/x11.c`, `check_fullscreen_messages()`;
  **8 failing assertions** (property and geometry for each invalid action and
  each client type).
- **Reproduce:**
  ```sh
  DWM_X11_STATE_FIXTURE=fullscreen sh src/tests/x11.sh "$PWD"
  ```
  Make a tiled or floating client fullscreen via ADD, then send a 32-bit
  `_NET_WM_STATE` message targeting fullscreen with action 3 or -1.
- **Expected:** Ignore unsupported actions and retain fullscreen and geometry.
- **Actual:** Fullscreen disappears and normal geometry/borders return.
- **Cause:** Any action other than ADD or applicable TOGGLE evaluates as REMOVE
  rather than being rejected. Valid add/remove/toggle and restoration controls
  pass.

## 5. Deleting a title property leaves a stale visible title

- **Severity:** Medium. The bar displays a title the application has removed,
  even after changing focus and redrawing.
- **Affected production file:** `src/dwm.c`, `propertynotify()`.
- **Failing test:** `src/tests/x11.c`, `check_title_updates()`;
  **3 failing pixel comparisons**.
- **Reproduce:**
  ```sh
  DWM_X11_STATE_FIXTURE=titles sh src/tests/x11.sh "$PWD"
  ```
  Give a managed client both `WM_NAME` and UTF-8 `_NET_WM_NAME`; delete
  `_NET_WM_NAME` and compare the bar with the legacy title's reference image.
  Focus away and back. Then set only `WM_NAME` and delete it.
- **Expected:** Fall back to `WM_NAME` when the preferred property is deleted;
  show the unnamed placeholder when the last title is removed.
- **Actual:** The preferred-title deletion and focus-redraw comparisons differ
  by 629 pixels each; deletion of the only legacy title differs by 537 pixels.
  The old title remains cached. Live replacements and empty-property recovery
  pass.
- **Cause:** `propertynotify()` returns early for `PropertyDelete` instead of
  updating the title.

## 6. Deleting WM_HINTS does not restore direct keyboard focus

- **Severity:** Medium. A client can remain unable to receive direct keyboard
  focus after removing the hint that disabled it.
- **Affected production file:** `src/dwm.c`, `propertynotify()` and
  `updatewmhints()`.
- **Failing test:** `src/tests/x11.c`, `check_focus_hints()`;
  **16 failing assertions** across eight initial hint/protocol combinations.
- **Reproduce:**
  ```sh
  DWM_X11_STATE_FIXTURE=focus-hints sh src/tests/x11.sh "$PWD"
  ```
  Map two clients; set the target's `WM_HINTS` to `InputHint=False`, remove its
  focus protocol, delete `WM_HINTS`, and cycle Super+j to the other client and
  back.
- **Expected:** Absence of the hint restores default direct focus. Keyboard
  focus and `_NET_ACTIVE_WINDOW` identify the target.
- **Actual:** Keyboard focus and `_NET_ACTIVE_WINDOW` remain on the other
  client in all eight combinations. Replacing the property with a hint lacking
  `InputHint` restores focus correctly.
- **Cause:** The deletion event is ignored, and `updatewmhints()` also fails to
  clear cached `neverfocus` when `XGetWMHints()` returns NULL.

## 7. Deleting WM_NORMAL_HINTS retains removed size constraints

- **Severity:** Medium. Floating applications cannot recover unconstrained
  resizing by removing their size-hint property.
- **Affected production file:** `src/dwm.c`, `propertynotify()`.
- **Failing test:** `src/tests/x11.c`, `check_size_hints()`;
  **1 failing size assertion**.
- **Reproduce:**
  ```sh
  DWM_X11_STATE_FIXTURE=size-hints sh src/tests/x11.sh "$PWD"
  ```
  Map a dialog-style floating client, install maximum size 140x100, delete
  `WM_NORMAL_HINTS`, request 217x173, then dispatch Super+t for a layout pass.
  The test confirms the property is actually absent.
- **Expected:** Removed constraints no longer affect the window's size.
- **Actual:** The client remains 140x100 instead of 217x173. Reinstalling an
  unconstrained property permits the requested size.
- **Cause:** `propertynotify()` ignores the deletion, leaving cached hints in
  force.

## 8. Initial-fullscreen dialogs and transients lose floating state on exit

- **Severity:** Medium. Dialogs and transient windows unexpectedly become tiled
  after leaving fullscreen, lose their ordinary size, and cannot accept an
  application resize.
- **Affected production file:** `src/dwm.c`, `manage()`, `updatewindowtype()`
  and `setfullscreen()`.
- **Failing test:** `src/tests/x11.c`, `check_initial_fullscreen_exit()`;
  **8 failing assertions** (exit geometry and resize for two client types and
  two exit actions).
- **Reproduce:**
  ```sh
  DWM_X11_STATE_FIXTURE=initial-fullscreen-exit sh src/tests/x11.sh "$PWD"
  ```
  Map a tiled barrier/parent, then a 300x200 client with either the dialog
  window type or `WM_TRANSIENT_FOR` pointing to the parent. Before mapping,
  set a 32-bit ATOM `_NET_WM_STATE` containing fullscreen alone. Verify full
  screen coverage, send repeated ADD, then send REMOVE (0) or TOGGLE (2).
  Request a 137x119 size after exit. Compare with the same client entering
  fullscreen by client message after an ordinary map.
- **Expected:** Exit preserves the client's floating nature, restores its
  300x200 size and configured border, and permits the 137x119 resize. Ordinary
  mapped clients and initial-fullscreen clients should behave equivalently.
- **Actual:** Both client types exit to tiled geometry `(416,33) 751x755 border
  1` on the isolated display and reject the resize, remaining 751x755. In the
  default suite's changed layout, they instead tile to `(30,33) 619x755`; the
  defect and assertion count are unchanged.
- **Cause:** `manage()` calls `updatewindowtype()` before resolving transient
  floating state. Within `updatewindowtype()`, fullscreen is processed before
  the dialog type. Thus `setfullscreen()` records `oldstate=0` before either
  kind of client is classified as floating. Fullscreen exit restores this
  stale state and `arrange()` tiles the client. Rules run earlier, so clients
  marked floating by a rule do not suffer this defect.
- **Coverage and controls:** REMOVE and TOGGLE exits, repeated ADD, all eight
  mapped-normal controls, initial-fullscreen tiled and rule-floating clients,
  state property removal, borders, focus and cleanup. All controls pass.
  Floating positions, fixed-size hints, hidden tags and monitor changes are
  intentionally not asserted. No resize is issued while fullscreen, isolating
  this finding from defect 3.

## Validation status and limits

- The seven state fixtures reproduce **75 total failing assertions**:
  24 + 15 + 8 + 3 + 16 + 1 + 8. They complete and clean up their clients/servers.
- The iteration 1 baseline `make test` passed the headless regression and build/documentation
  checks and the X11 switcher appearance/live-reload check, then exited nonzero
  during switcher navigation before reaching the state fixtures. Its output
  included `error, cannot allocate color '#fffffg'` from the intentional invalid
  theme exercise and `X connection ... broken`; the navigation exit itself has
  not been diagnosed. This is a pre-existing suite blocker, not evidence that
  the new state tests passed or caused the exit.
- Validation with `DWM_X11_STATE_FIXTURE=fullscreen-state-preservation make test`
  passed every headless check and the X11 event-loop check, then failed with the
  same 24 new assertions (make exit status 2). A forced production rebuild with
  `make -B` succeeded. `sh -n src/tests/x11.sh`, `git diff --check`, and rejection
  of an unknown fixture selector passed. No test processes remain running.
- Existing production compiler warnings are outside this test-only change;
  they were not suppressed or fixed. No production fixes or commits were made.
- Iteration 2 default `make test` passed the headless checks, both XTEST
  switcher checks and the event-loop check, then reached all seven state
  fixtures and reported the same **75 failures** (make exit status 2). The
  earlier navigation blocker is therefore intermittent, not a consistent
  prerequisite failure; its cause remains undiagnosed. Later integration
  checks remain blocked by the genuine state failures.
- Iteration 2 isolated `DWM_X11_STATE_FIXTURE=initial-fullscreen-exit make test`
  passed every headless and event-loop check, then reproduced exactly the
  **8 new assertions** (make exit status 2), matching the direct isolated run.
  No relevant dependency checks skipped. `make -B` succeeded; the Xlib test
  client compiled with `-std=c99 -pedantic -Wall -Wextra -Werror`; shell syntax
  and diff-whitespace checks passed. No production source was modified, no
  commits were made, and no test processes remain running.
- Long-title truncation, alternate title encodings, fixed-size initial
  fullscreen clients, floating exit positions, physical input and side-by-side
  monitor behavior are not covered by the new fixture.
