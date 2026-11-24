# Reproduced defects

Production code is unchanged. All assertions below remain enabled and fail
normally, without expected-failure masks. Severity is assessed by user impact:
Medium means incorrect window-manager behavior with a recoverable workaround.
These findings were reproduced against the current production sources in a
`DWM_TEST` build, which disables desktop autostart only.

## Reproduction environment

Run from the repository root with the build dependencies in `src/config.mk`,
`jq`, `pkg-config`, Xvfb, `xdpyinfo` and `xrdb` installed. XTEST development
files were available during validation, but these nine state fixtures use Xlib
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

## 9. Malformed client-message formats change fullscreen state

- **Severity:** Medium. An incorrectly encoded application request can enter or
  exit fullscreen unexpectedly. This is a protocol-validation defect, not an
  authorization bypass: X11 clients can already send valid fullscreen requests.
- **Affected production file:** `src/dwm.c`, `clientmessage()`.
- **Failing test:** `src/tests/x11.c`,
  `check_fullscreen_message_formats()`; **64 failing assertions** (state and
  geometry for 32 malformed requests that should not change the window).
- **Reproduce:**
  ```sh
  DWM_X11_STATE_FIXTURE=fullscreen-message-formats sh src/tests/x11.sh "$PWD"
  ```
  Map a tiled barrier and a tiled or dialog-style floating target. Establish
  normal or fullscreen state with valid 32-bit messages. Send a ClientMessage
  of type `_NET_WM_STATE` with format 8 or 16, containing 20 payload bytes that
  resemble `data.l` action and fullscreen atom values in either atom slot.
  Specifically, copy the first 20 bytes of the native `long[5]` array
  `{action, fullscreen, None, 0, 0}` or
  `{action, unrelated, fullscreen, 0, 0}` into `ev.xclient.data.b` before
  `XSendEvent()` to the root with the substructure masks. Send ADD or TOGGLE
  while normal, or REMOVE or TOGGLE while fullscreen. A marked tiled
  ConfigureRequest reply orders the assertion after dwm handles the message.
- **Expected:** `_NET_WM_STATE` requests have format 32. Ignore other formats,
  preserving state, geometry, borders and focus regardless of payload bytes.
- **Actual:** On the validated native 64-bit X11 environment, both malformed
  formats act like valid requests. For example, tiled format 8 slot 1 ADD
  changes the property from absent to fullscreen and geometry to
  `(0,0) 1200x800 border 0`; format 16 REMOVE clears fullscreen and restores
  `(416,33) 751x755 border 1`. Both atom slots and both client types fail.
- **Cause:** `clientmessage()` reads `cme->data.l` without checking
  `cme->format == 32`. Bytes from the other union representations are
  interpreted as EWMH action and atom values.
- **Coverage and controls:** All three actions from both starting states,
  formats 8/16/32, both atom slots and both client types: 72 requests total.
  All 24 equivalent format-32 controls and 16 malformed no-op requests pass.
  Valid requests reset state before every case, and reset assertions pass,
  preventing a malformed ADD from concealing a malformed REMOVE. Focus,
  border/geometry controls, marked round trips and client cleanup pass.
  No unrelated state property is seeded, and no resize is sent to the target
  while fullscreen, isolating this defect from findings 1 and 3. Payloads are
  intentionally native union-shaped; cross-endian clients, invalid formats
  other than 8/16 are not covered here; malformed `_NET_ACTIVE_WINDOW` requests
  are covered by finding 10.

## 10. Malformed activation requests spuriously mark background clients urgent

- **Severity:** Medium. An incorrectly encoded activation message causes a
  false urgency indication for a background application. This shares the
  missing format validation of finding 9, but affects a separate message type
  and user-visible state. It is not an authorization bypass.
- **Affected production file:** `src/dwm.c`, `clientmessage()` activation branch
  and `seturgent()` (which writes the resulting urgency to `WM_HINTS`).
- **Failing test:** `src/tests/x11.c`, `check_active_window_messages()`;
  **12 failing assertions**, two requests each to six fresh background clients.
- **Reproduce:**
  ```sh
  DWM_X11_STATE_FIXTURE=active-window-messages sh src/tests/x11.sh "$PWD"
  ```
  Map a tiled barrier and target. Give the target `WM_HINTS` with `InputHint`
  true and `StateHint=NormalState`, without `XUrgencyHint`. Focus the barrier
  using Super+j. Send a ClientMessage to the root using the substructure masks,
  with `window=target`, `message_type=_NET_ACTIVE_WINDOW`, and format 8 or 16.
  Fill its 20 payload bytes with `0xa5`, then set `data.b[0]` (format 8) or
  `data.s[0]` (format 16) to source 0, 1 or 2. Wait for a marked tiled
  ConfigureRequest reply and read the target's `WM_HINTS`. Send the request
  again and check the hint independently.
- **Expected:** Activation requests have format 32. Ignore malformed formats
  without changing urgency, keyboard focus, root `_NET_ACTIVE_WINDOW`, geometry,
  borders or unrelated hints. A valid 32-bit request to a background client
  should follow dwm's intentional no-focus-stealing policy and set urgency.
- **Actual:** Both malformed formats add `XUrgencyHint`, for every tested source
  indication, on the first request and retain it on the second. For example,
  `format 8 source 0 background request 1: urgency is 1, expected 0`.
  Keyboard focus and root `_NET_ACTIVE_WINDOW` correctly stay on the barrier.
- **Cause:** The `_NET_ACTIVE_WINDOW` branch checks only that the target is not
  selected and not already urgent. It calls `seturgent(c, 1)` without checking
  `cme->format`; unlike the fullscreen reproduction, no special payload is
  necessary because the activation branch never reads the data fields.
- **Coverage and controls:** 54 requests across formats 8/16/32, sources 0/1/2,
  selected/nonurgent-background/already-urgent-background states and repeated
  requests. Each of 27 clients starts with independent hints and cached state.
  All 18 valid-format controls and 24 malformed selected/already-urgent requests
  pass. Unrelated hint flags, input/state fields, focus, active-window property,
  geometry, borders, clearing urgency on user focus and cleanup pass. Absent
  hints, hidden tags, unmanaged targets, invalid sources, timestamps and bar
  urgency pixels are not asserted.

## Validation status and limits

- The nine state fixtures reproduce **151 total failing assertions**:
  24 + 15 + 8 + 3 + 16 + 1 + 8 + 64 + 12. They complete and clean up their clients/servers.
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
- Iteration 3 default `make test` passed every headless check, both XTEST
  switcher checks and the event-loop check, then reproduced **139 failures**
  across all eight state fixtures (make exit status 2). Isolated
  `DWM_X11_STATE_FIXTURE=fullscreen-message-formats make test` passed the
  headless and event-loop checks and reproduced **64 failures**, matching the
  direct isolated script run. No relevant dependency checks skipped.
  `make -B` succeeded, the Xlib client compiled with
  `-std=c99 -pedantic -Wall -Wextra -Werror`, and shell syntax and
  diff-whitespace checks passed. No production code was changed, no commits
  were made, and no test processes remain running. Later integration checks
  remain blocked by the preserved state failures.
- Iteration 4 default `make test` passed every headless check, both XTEST
  switcher checks and the event-loop check, then reproduced **151 failures**
  across all nine state fixtures (make exit status 2). Isolated
  `DWM_X11_STATE_FIXTURE=active-window-messages make test` passed the headless
  and event-loop checks and reproduced exactly **12 failures**, matching the
  direct isolated script reproduction. No relevant dependency checks skipped.
  `make -B`, strict `-std=c99 -pedantic -Wall -Wextra -Werror` compilation of
  the Xlib client, shell syntax and diff-whitespace checks passed. Production
  sources are unchanged; existing production warnings remain. No commits were
  made and no test processes remain running. Later integration checks remain
  blocked by the preserved state failures.
- Long-title truncation, alternate title encodings, fixed-size initial
  fullscreen clients, floating exit positions, physical input and side-by-side
  monitor behavior remain untested. The activation fixture's additional limits
  are listed under finding 10.
