# Regression checks

From the repository root, run `make test` with GCC or Clang, C99 support,
`pkg-config`, the configured X11, Xft, fontconfig, XCB, and Xinerama development
headers/libraries, and the system math library available. The test target
compiles temporary executables from the production sources. It covers layout
geometry and gap branches, core monitor/client state helpers (including
refusing scratchpad-only tag views, reading the parent of a process from
`/proc/<pid>/stat` for swallowing when the process names itself with spaces
and parentheses, rejecting Xresources strings that exceed
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
dependencies and the LICENSE file shipped by `make dist`, and that `make install`
and the Arch package's install and upgrade scriptlet give `dwmsysmon` the
`cap_perfmon=p` (permitted only) capability with a stub `setcap`, and only print how to set it
when that fails, without failing themselves.

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
delivery, transient-window placement, and `_NET_WM_STATE` read only as a
32-bit ATOM: a 32-bit `_NET_WM_STATE_FULLSCREEN` makes a window fullscreen,
while 8-bit data holding the bytes of that atom, or a single byte, does not.
`_NET_CLIENT_INFO` set before a window maps is likewise read only as two
32-bit CARDINALs: 8-bit data leaves the window shown on the current tags, tags
outside the tag mask are dropped, and a 32-bit tag 2 is still restored.
Negative size hints (minimum, maximum, increment and zero aspect) must leave a
floating window at its requested size, and a root ConfigureNotify without a
size, which any client can send, must leave the bar as it was.
Autostart is disabled only in the `dwm` test build, so no configured desktop
command is launched.

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
no value), the sampler's histories (the latest 16 readings, oldest first, the
oldest dropped when it is full, and cleared by a missing or out-of-range
reading; the CPU and the GPU usage each have their own), and the property
payload (`cpu=<0-100>`, `temp=<0-150>`, `fan=<rpm>[,<rpm>...]` with up to four
fans of 0-30000, `cpuhist=<0-100>[,<0-100>...]` with up to 16 samples, and
`gpu=<0-100>` and `gpuhist=` like `cpuhist=`, in at most 256 bytes;
oversized, empty and NUL-containing payloads are no value, and a malformed,
out-of-range or repeated field, such as a history with an empty, signed,
four-digit or out-of-range sample or a 17th one, is missing while the others
stay; every combination of values and history lengths survives encoding and
decoding, the longest payload fits, and without a GPU still fits the 128 bytes a
dwm from before the GPU usage reads, encoding leaves out a usage or history with
a sample out of range or too many samples rather than sending part of it, a
buffer one byte short of the longest payload is no payload, and the decoder dwm
had before temperatures and fans still reads the CPU usage of a new sampler). It
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
slower cadence for slow reads. For the GPU usage it builds fake DRM trees and
replaces `read()` in the check, so that every read of a GPU's usage or of an
Intel GPU's counter (what would wake a suspended GPU) is counted and can fail
with EBUSY, and `perf_event_open` (through `syscall()`) with a fake i915 PMU
whose counters are files holding the busy and enabled time, which opens fail
with a set error, or with EPERM unless `CAP_PERFMON` was made effective by a
`capget`/`capset` pair that is refused when it is not permitted: no GPU, or
none with a usage (connectors, render nodes and a card without a source are not
used), amdgpu's `gpu_busy_percent` (malformed and out-of-range readings are no
value), each `power/runtime_status` (read when `active` or `unsupported`; idle
with no read when `suspended` or `suspending`; no value and no read when
resuming, in error, malformed or missing), a read failing with EBUSY or a usage
that cannot be read (no value, and looked for again), an Intel GPU's engines
(the `*-busy` events of its PMU with an engine config, other events and
malformed configs left out, at most 16 in order of class and instance; its
counters opened system-wide on the first CPU of the PMU's cpumask, with the
enabled time, only at the start, even while it is suspended, without a read,
and never by a later look for GPUs; no counters, never shown, asleep or
awake; the busiest engine's share of the enabled time, rounded, at most 100%;
asleep idle with no read, and the first reading since it woke up, like the
very first, pending; counters kept open when the GPUs are looked for again and
closed, without new ones, when the engines change or the GPU goes away;
counters that stop or go backwards no value, counting again from there,
counters that cannot be read closed for good, and counters that cannot all be
opened at the start none left open; EACCES, EPERM and other open errors never
showing the GPU; a pending reading publishing no usage while the history goes
on; xe, a missing driver, a PMU without a type or of type 0 or without engines
not used; a discrete card's PMU named after its PCI device beside the
integrated one's, and a cpumask range or none),
a hybrid laptop (the busier GPU, the suspended one idle and not read, one
failing with EBUSY beside one read, an awake one beside a suspended Intel GPU,
a pending Intel GPU beside a suspended one, and an awake amdgpu listed before
an Intel GPU that just woke up, whose pending reading hides nothing), `CAP_PERFMON` made effective
once at the start when permitted, and, when it is not (a container, an old
kernel), the counters not opened, the Intel GPU never shown and an amdgpu beside
it, with the CPU, still read, giving up every capability for
good (`capset` asked to clear them all, and `NoNewPrivs` set and no ambient
capability left in `/proc/self/status`, also when the check runs as root), more cards than are read, the
delay between reads (never within the longest autosuspend delay of the GPUs
plus a second, a delay that cannot be read or is negative, the sampler's
interval at least, 5% of the time for slow reads, no overflow), the history and
when GPUs are looked for again, and paths cut short. `core.c` also checks the section's text with and
without each field, split where the graphs go (the GPU usage last, `-` for a
GPU history without its usage), that its width follows the
fields present but not their values, and that it is centred and left out, never
overlapping the layout symbol or the status, on wide and narrow bars and at the
exact boundaries; with the graph (16 bars of `sysmongraph` / 16 px, 1 px at
least, none for a width of 0 or below) the section is wider by the graph, and
by a second one with a GPU usage or history, and
when room runs short the graphs lose the same bars one at a time down to none before the
section is left out, at the exact pixel where the status begins and where the
title would keep only a bar height. It checks that a held value, history
included, expires on time and that dwm sets no poll timeout without one, and that
a click on the section, its
graphs (the GPU's included) or the gap up to the status is neither a title, layout symbol nor status
click, while a click on the title left of it, on the status, or with the section
off or left out for lack of room keeps its usual meaning.

`sysmon-x11.c` runs from `x11.sh` against the running test dwm and recognises
the system monitor section by the bar pixels it produces. It enables the
section through Xresources and SIGUSR2, and uses itself as a stand-in sampler to
check that values show up and differ, that the temperature and every fan show
up and change the bar, that a malformed temperature or fan list is left out
without hiding the other fields, that garbage (wrong content, wrong type or
format, oversized, empty, removed) shows `-`, that a value nobody refreshes goes
stale by itself and stays while it is refreshed, and after each of these that
dwm still redraws a status change. For the graph it checks that it is 32 px
wide between the CPU usage and the other fields, with no text pixel in or next
to its columns; that full bars are all as high, on one bottom row; that a
history of 100% then 0% is a full bar left of a 1 px line at the right end and
a single 50% sample half a bar there, with the rest of the graph empty, and a
99% sample a full bar (heights are rounded to the nearest pixel); that a
malformed history (empty, out of range, signed, four digits, 17 samples,
repeated and more) draws nothing and leaves the other fields where they are;
that a history goes stale with its value; and, with the status a red rectangle
of a width searched for, that a status taking the graph's room cuts it to whole
bars, keeping the newest, while every pixel of the status stays red and the
section keeps at least as far from the status as the graph is from the CPU
usage. With a GPU usage it checks that the usage shows and its value changes the
bar, that a malformed usage or history is left out on its own, that a GPU
history without its usage still draws, that the GPU graph is 32 px wide after
the CPU graph and the other fields, with no text in or next to it, its full bars
as high and on the same row as the CPU's, a history of 100% then 0% a full bar
left of a line, and the CPU graph untouched by it; and that a status taking
the room cuts both graphs to the same whole bars, the GPU's keeping its newest
and its gap from the status, which stays red. The test dwm finds `dwmsysmon` through a
symbolic link first on its `PATH`, which the check points at a stub that exits
at once while it stands in for the sampler, and at the fixture build of the real
sampler otherwise; samplers are counted by their executable and `DISPLAY` in
`/proc`, so an installed `dwmsysmon` is never started or counted. It also checks that the section is centred
in the bar with a short and with a long status text, and is left out, without
overlapping anything, when a very long status text reaches the centre, with
the CPU usage alone, with the temperature and one, two or four fans, and with a
GPU usage after them. It first
checks that dwm, running with the section off by default, started no sampler.
It then shows the section so that dwm starts the real `dwmsysmon`, built with `SYSMON_STAT_PATH` pointing at a fixture
instead of `/proc/stat` and `SYSMON_SYS_PATH` at a fake sysfs tree instead of
`/sys`, and sampling at the default 4000 ms interval (with `dwm.sysmonstale` at
9000), and checks the published value and the bar for a changing load (the
bar outside the graph's columns, which the stand-in measures for each set of
fields, since the graph follows the sampler's history), that the history
holds 20%, 80% and 20% in that order and ends with the published usage, and
that dwm draws it exactly as it draws the same payload from the stand-in, that it
publishes every 4000 ms as dwm tells it, every 1000 ms when another interval is
published in dwm's place, and every 4000 ms again for an interval below 250 or
above 60000 ms, a stopped (hung) sampler, a malformed and a missing stat file
(`-`, then recovery), a hidden section (the sampler exits and leaves no value,
and showing the section again starts a new one whose value comes back), a
sampler stopped with SIGSTOP while the section is hidden (dwm hides the section
and stays responsive, and when the section is shown again the sampler dwm
starts retires the stopped one once it continues), the sampler dwm starts holding no capability and unable to gain
any (`/proc/<pid>/status`), sensors appearing in the fake tree (the temperature
and both fans of a Dell-like laptop, then one fan, none, and a malformed
temperature, each published and drawn), a sensor read that hangs (a FIFO as the
temperature input: dwm stays responsive and lets the value go stale, and the
value returns once the read completes), an amdgpu card in the fake tree (its
usage published and changing, its history holding the readings in order, and
dwm drawing the sampler's payload exactly as the stand-in's; while
`runtime_status` is `suspended`, published as idle while the CPU usage goes on,
with `gpu_busy_percent` a FIFO nobody ever opens; then active again with an
`autosuspend_delay_ms` of 10000, read at most every third 4000 ms sample so the
reads never keep it awake, and gone, after which no GPU is published or drawn;
then an i915 card with a PMU whose counters the sampler cannot open, as without
`CAP_PERFMON`, never published or drawn, awake or asleep), a sampler started by hand taking over
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
publishes the flag the sampler exits on. It also checks that dwm publishes the
4000 ms sampling interval of `config.h` and that the default staleness limit
covers more than two samples.
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
