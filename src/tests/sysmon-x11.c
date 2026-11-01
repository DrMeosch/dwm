/*
 * End-to-end checks of the system monitor section against the running test dwm:
 * a stand-in sampler (this program) and the real dwmsysmon publish through the
 * root window property, and the bar must show the value, or "-" when the data
 * is missing, malformed or stale, while dwm stays responsive. The section is
 * recognised by the bar pixels it produces.
 *
 * usage: sysmon-x11 <dwm pid> <dwmsysmon> <scratch directory>
 *
 * dwmsysmon must be built with SYSMON_STAT_PATH set to <scratch directory>/stat.
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#ifdef HAVE_XTEST
#include <X11/extensions/XTest.h>
#endif
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STALE_MS 1500 /* dwm.sysmonstale used with the stand-in sampler */
#define SAMPLER_STALE_MS 4500 /* dwm.sysmonstale used with dwmsysmon, which samples every 2000 ms */

static Display *dpy;
static Window root;
static Atom sysmonatom, enabledatom;
static pid_t dwm_pid, sampler_pid;
static int failures;
static int fixture_busy = 20; /* percent busy in the stat file the stand-in kernel writes */
static unsigned long long fixture_total, fixture_idle;
static char fixture_path[512];
static int fixture_frozen; /* leave the stat file alone, e.g. while it is deliberately broken */

#define CHECK(condition, ...) do { \
	if (!(condition)) { \
		fprintf(stderr, "FAIL: "); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
		failures++; \
	} \
} while (0)

static void
pause_millis(long millis)
{
	struct timeval delay = { millis / 1000, (millis % 1000) * 1000 };

	select(0, NULL, NULL, NULL, &delay);
}

/* Advance the fake /proc/stat by 25 ticks at the current busy share; replaced atomically. */
static void
tick_fixture(void)
{
	char tmp[600], line[128];
	FILE *fp;

	if (fixture_frozen)
		return;
	fixture_total += 25;
	fixture_idle += 25 - 25 * fixture_busy / 100;
	snprintf(tmp, sizeof tmp, "%s.new", fixture_path);
	if (!(fp = fopen(tmp, "w")))
		return;
	snprintf(line, sizeof line, "cpu  %llu 0 0 %llu 0 0 0 0\ncpu0 1 2 3 4\n",
	         fixture_total - fixture_idle, fixture_idle);
	fputs(line, fp);
	fclose(fp);
	rename(tmp, fixture_path);
}

static void
pump(long millis)
{
	tick_fixture();
	pause_millis(millis);
}

static Window
find_bar(void)
{
	Window r, parent, *children = NULL, bar = None;
	XWindowAttributes wa;
	unsigned int count, i;

	if (!XQueryTree(dpy, root, &r, &parent, &children, &count))
		return None;
	for (i = 0; i < count && bar == None; i++)
		if (XGetWindowAttributes(dpy, children[i], &wa) && wa.override_redirect
		&& wa.map_state == IsViewable && wa.x == 0 && wa.y == 0
		&& wa.width == DisplayWidth(dpy, DefaultScreen(dpy)) && wa.height < 40)
			bar = children[i];
	if (children)
		XFree(children);
	return bar;
}

/* A hash of everything the bar currently shows. */
static unsigned long
bar_hash(void)
{
	Window bar = find_bar();
	XWindowAttributes wa;
	XImage *image;
	unsigned long h = 14695981039346656037UL;
	int x, y;

	if (bar == None || !XGetWindowAttributes(dpy, bar, &wa)
	|| !(image = XGetImage(dpy, bar, 0, 0, wa.width, wa.height, AllPlanes, ZPixmap)))
		return 0;
	for (y = 0; y < wa.height; y++)
		for (x = 0; x < wa.width; x++)
			h = (h ^ XGetPixel(image, x, y)) * 1099511628211UL;
	XDestroyImage(image);
	return h;
}

/* Wait until the bar shows exactly what hash describes. */
static int
wait_hash(unsigned long hash, long millis)
{
	long waited;

	for (waited = 0; waited <= millis; waited += 25) {
		if (bar_hash() == hash)
			return 1;
		pump(25);
	}
	return 0;
}

static void
set_status(const char *text)
{
	XStoreName(dpy, root, text);
	XFlush(dpy);
}

/* The bar is steady: it drew the same thing twice in a row. */
static unsigned long
steady_hash(void)
{
	unsigned long a, b;
	int i;

	for (i = 0; i < 80; i++) {
		a = bar_hash();
		pump(40);
		b = bar_hash();
		if (a && a == b)
			return a;
	}
	return 0;
}

/* dwm still serves events: a status change is drawn, and so is changing it back. */
static int
responsive(const char *what)
{
	static int n;
	char text[64];
	unsigned long before = bar_hash(), during = 0;
	int i, ok = 0;

	snprintf(text, sizeof text, "probe %d", ++n);
	set_status(text);
	for (i = 0; i < 80 && !ok; i++) {
		pump(25);
		during = bar_hash();
		ok = during != before;
	}
	set_status("sysmon-base");
	CHECK(ok, "dwm did not redraw after %s", what);
	CHECK(wait_hash(before, 2000), "dwm did not settle again after %s", what);
	CHECK(kill(dwm_pid, 0) == 0, "dwm died after %s", what);
	return ok;
}

static void
publish(const char *text)
{
	XChangeProperty(dpy, root, sysmonatom, XA_STRING, 8, PropModeReplace,
	                (const unsigned char *)text, (int)strlen(text));
	XFlush(dpy);
}

static void
remove_value(void)
{
	XDeleteProperty(dpy, root, sysmonatom);
	XFlush(dpy);
}

/* Read the sampler's property as dwm would; returns 1 and fills buf when it is a string. */
static int
read_value(char *buf, size_t size)
{
	Atom type;
	int format, ok = 0;
	unsigned long n, after;
	unsigned char *data = NULL;

	if (XGetWindowProperty(dpy, root, sysmonatom, 0, 16, False, XA_STRING, &type,
	    &format, &n, &after, &data) == Success && data) {
		if (type == XA_STRING && format == 8 && n < size) {
			memcpy(buf, data, n);
			buf[n] = '\0';
			ok = 1;
		}
		XFree(data);
	}
	return ok;
}

static int
wait_value(const char *want, long millis)
{
	char buf[64];
	long waited;

	for (waited = 0; waited <= millis; waited += 25) {
		if (want ? (read_value(buf, sizeof buf) && !strcmp(buf, want))
		         : !read_value(buf, sizeof buf))
			return 1;
		pump(25);
	}
	return 0;
}

static long
enabled_flag(void)
{
	Atom type;
	int format;
	unsigned long n, after;
	unsigned char *data = NULL;
	long on = -1;

	if (XGetWindowProperty(dpy, root, enabledatom, 0, 1, False, XA_CARDINAL, &type,
	    &format, &n, &after, &data) == Success && data) {
		if (type == XA_CARDINAL && format == 32 && n == 1)
			on = (long)*(unsigned long *)data;
		XFree(data);
	}
	return on;
}

static void
set_resources(const char *text)
{
	FILE *fp = popen("xrdb -merge", "w");

	if (!fp) {
		CHECK(0, "cannot run xrdb");
		return;
	}
	fputs(text, fp);
	pclose(fp);
	kill(dwm_pid, SIGUSR2);
}

static long
context_switches(pid_t pid)
{
	char path[64], line[128];
	FILE *fp;
	long n = -1;

	snprintf(path, sizeof path, "/proc/%ld/status", (long)pid);
	if (!(fp = fopen(path, "r")))
		return -1;
	while (fgets(line, sizeof line, fp))
		if (sscanf(line, "voluntary_ctxt_switches: %ld", &n) == 1)
			break;
	fclose(fp);
	return n;
}

static void
start_sampler(const char *path)
{
	if ((sampler_pid = fork()) == 0) {
		execl(path, path, (char *)NULL);
		_exit(127);
	}
	CHECK(sampler_pid > 0, "cannot start dwmsysmon");
}

/* The pixels for the value (or "-"), measured with the stand-in sampler. */
static unsigned long
reference(const char *payload)
{
	unsigned long h;

	if (payload)
		publish(payload);
	else
		remove_value();
	pump(25);
	h = steady_hash();
	return h;
}

int
main(int argc, char *argv[])
{
	unsigned long h_off, h_dash, h20, h80, h42;
	char big[4096];
	long before, after;
	unsigned long wrongtype[1] = { 42 };
	unsigned short wide[8] = { 'c', 'p', 'u', '=', '4', '2' };
	int i, status, quiet;
	static const struct { const char *text; int len; } garbage[] = {
		{ "cpu=abc", 7 }, { "cpu=101", 7 }, { "cpu=-5", 6 }, { "cpu=", 4 }, { "cpu", 3 },
		{ "junk", 4 }, { "\377\377\377\377", 4 }, { "cpu=42x", 7 }, { "cpu=1000000", 11 },
		{ "cpu=42\0cpu=43", 13 }, { "", 0 }, { "\033[2J\033]0;x\007", 10 },
	};

	if (argc != 4)
		return 2;
	dwm_pid = (pid_t)atol(argv[1]);
	snprintf(fixture_path, sizeof fixture_path, "%s/stat", argv[3]);
	if (!(dpy = XOpenDisplay(NULL))) {
		fputs("sysmon-x11: cannot open display\n", stderr);
		return 1;
	}
	root = DefaultRootWindow(dpy);
	sysmonatom = XInternAtom(dpy, "_DWM_SYSMON", False);
	enabledatom = XInternAtom(dpy, "_DWM_SYSMON_ENABLED", False);
	fixture_total = 1000;
	fixture_idle = 1000;
	tick_fixture();

	/* hidden by default: nothing is drawn, whatever is published */
	set_status("sysmon-base");
	h_off = steady_hash();
	CHECK(h_off, "the bar never settled");
	CHECK(enabled_flag() == 0, "dwm did not tell the sampler the section is hidden (%ld)", enabled_flag());
	publish("cpu=42");
	pump(300);
	CHECK(bar_hash() == h_off, "a value published while the section is hidden changed the bar");
	remove_value();

	/* shown by the resource: the empty section is "-" and takes room from the title */
	/* a long stale limit: a payload dwm wrongly accepted must not vanish by itself and pass as "-" */
	set_resources("dwm.showsysmon: 1\ndwm.sysmonstale: 60000\n");
	for (i = 0; i < 80 && bar_hash() == h_off; i++)
		pump(25);
	h_dash = steady_hash();
	CHECK(h_dash && h_dash != h_off, "enabling the section through Xresources did not change the bar");
	CHECK(enabled_flag() == 1, "dwm did not tell the sampler the section is shown (%ld)", enabled_flag());

	/* values come through, and different values look different */
	h20 = reference("cpu=20");
	h80 = reference("cpu=80");
	h42 = reference("cpu=42");
	CHECK(h20 && h80 && h42 && h20 != h80 && h20 != h42 && h80 != h42 && h20 != h_dash && h42 != h_dash,
	      "published values do not look distinct on the bar");
	CHECK(reference("cpu=42 gpu=9 something=else") == h42, "unknown fields in the payload changed the value");

	/* garbage in the property: "-", and dwm keeps working */
	for (i = 0; i < (int)(sizeof garbage / sizeof *garbage); i++) {
		publish("cpu=42");
		CHECK(wait_hash(h42, 2000), "the value was not shown before garbage %d", i);
		XChangeProperty(dpy, root, sysmonatom, XA_STRING, 8, PropModeReplace,
		                (const unsigned char *)garbage[i].text, garbage[i].len);
		XFlush(dpy);
		CHECK(wait_hash(h_dash, 2000), "garbage payload %d was not shown as \"-\"", i);
		responsive("garbage");
	}
	memset(big, ' ', sizeof big);
	memcpy(big, "cpu=42", 6);
	publish("cpu=42");
	CHECK(wait_hash(h42, 2000), "the value was not shown before an oversized payload");
	XChangeProperty(dpy, root, sysmonatom, XA_STRING, 8, PropModeReplace,
	                (const unsigned char *)big, (int)sizeof big);
	XFlush(dpy);
	CHECK(wait_hash(h_dash, 2000), "an oversized payload was not shown as \"-\"");
	publish("cpu=42");
	CHECK(wait_hash(h42, 2000), "the value was not shown before a wrong-type payload");
	XChangeProperty(dpy, root, sysmonatom, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)wrongtype, 1);
	XFlush(dpy);
	CHECK(wait_hash(h_dash, 2000), "a CARDINAL payload was not shown as \"-\"");
	publish("cpu=42");
	CHECK(wait_hash(h42, 2000), "the value was not shown before a wrong-format payload");
	XChangeProperty(dpy, root, sysmonatom, XA_STRING, 16, PropModeReplace, (unsigned char *)wide, 6);
	XFlush(dpy);
	CHECK(wait_hash(h_dash, 2000), "a 16-bit payload was not shown as \"-\"");
	publish("cpu=42");
	CHECK(wait_hash(h42, 2000), "the value was not shown before the property was removed");
	remove_value();
	CHECK(wait_hash(h_dash, 2000), "a removed value was not shown as \"-\"");
	responsive("garbage payloads");

	/* a value nobody refreshes goes stale by itself, and dwm then sleeps without a timer */
	set_resources("dwm.sysmonstale: 1500\n");
	pump(300);
	publish("cpu=42");
	CHECK(wait_hash(h42, 2000), "the value was not shown before it went stale");
	pump(400);
	CHECK(bar_hash() == h42, "a fresh value was dropped early");
	CHECK(wait_hash(h_dash, STALE_MS + 3000), "a value that was never refreshed did not go stale");
	responsive("a stale value");
	publish("cpu=80");
	CHECK(wait_hash(h80, 2000), "a new value after staleness was not shown");
	for (i = 0; i < 8; i++) { /* a sampler that keeps refreshing keeps the value */
		pump(400);
		publish("cpu=80");
	}
	CHECK(bar_hash() == h80, "a refreshed value went stale");
	remove_value();
	CHECK(wait_hash(h_dash, 2000), "the value did not go away");

	/* the real sampler: values from a stat file, then hang, bad data and no data */
	set_resources("dwm.sysmonstale: 4500\n");
	start_sampler(argv[2]);
	fixture_busy = 20;
	CHECK(wait_hash(h20, 6000), "dwmsysmon's 20%% was not shown by dwm");
	CHECK(wait_value("cpu=20", 3000), "dwmsysmon did not publish cpu=20");
	fixture_busy = 80;
	CHECK(wait_hash(h80, 6000), "dwmsysmon's 80%% was not shown by dwm");
	fixture_busy = 20;
	CHECK(wait_hash(h20, 6000), "dwmsysmon's change back to 20%% was not shown by dwm");

	kill(sampler_pid, SIGSTOP); /* a hung sampler */
	CHECK(wait_hash(h_dash, SAMPLER_STALE_MS + 3000), "a hung sampler's value did not go stale");
	responsive("a hung sampler");
	kill(sampler_pid, SIGCONT);
	CHECK(wait_hash(h20, 6000), "the value did not return after the sampler resumed");

	{ /* unreadable and malformed stat files give "-", and recover */
		FILE *fp = fopen(fixture_path, "w");

		fixture_frozen = 1;
		if (fp) {
			fputs("cpu  not numbers at all\n", fp);
			fclose(fp);
		}
	}
	CHECK(wait_hash(h_dash, 4000), "a malformed stat file was not shown as \"-\"");
	CHECK(wait_value(NULL, 1000), "dwmsysmon kept publishing for a malformed stat file");
	responsive("a malformed stat file");
	unlink(fixture_path);
	pause_millis(1200);
	CHECK(bar_hash() == h_dash, "a missing stat file was not shown as \"-\"");
	CHECK(kill(dwm_pid, 0) == 0, "dwm died with the sampler's data missing");
	fixture_frozen = 0;
	CHECK(wait_hash(h20, 8000), "the value did not return after the stat file came back");

	/* hidden again: the sampler publishes nothing and stops waking up */
	set_resources("dwm.showsysmon: 0\n");
	CHECK(wait_hash(h_off, 3000), "turning the section off through Xresources did not restore the bar");
	CHECK(wait_value(NULL, 3000), "the sampler left its value behind while hidden");
	CHECK(enabled_flag() == 0, "dwm did not tell the sampler the section is hidden again");
	quiet = 0;
	for (i = 0; i < 4 && !quiet; i++) {
		before = context_switches(sampler_pid);
		pause_millis(1500);
		after = context_switches(sampler_pid);
		quiet = before >= 0 && before == after;
	}
	CHECK(quiet, "dwmsysmon keeps waking up while the section is hidden");
	set_resources("dwm.showsysmon: 1\n");
	CHECK(wait_hash(h20, 8000), "the value did not return when the section was shown again");

#ifdef HAVE_XTEST
	{ /* Mod+Shift+b toggles the section */
		KeyCode super = XKeysymToKeycode(dpy, XK_Super_L);
		KeyCode shift = XKeysymToKeycode(dpy, XK_Shift_L);
		KeyCode b = XKeysymToKeycode(dpy, XK_b);
		int press;

		CHECK(super && shift && b, "X server keymap is missing a required test keysym");
		for (press = 0; press < 2; press++) {
			XTestFakeKeyEvent(dpy, super, True, CurrentTime);
			XTestFakeKeyEvent(dpy, shift, True, CurrentTime);
			XTestFakeKeyEvent(dpy, b, True, CurrentTime);
			XTestFakeKeyEvent(dpy, b, False, CurrentTime);
			XTestFakeKeyEvent(dpy, shift, False, CurrentTime);
			XTestFakeKeyEvent(dpy, super, False, CurrentTime);
			XFlush(dpy);
			if (press == 0) {
				CHECK(wait_hash(h_off, 3000), "Mod+Shift+b did not hide the section");
				CHECK(enabled_flag() == 0, "Mod+Shift+b did not tell the sampler to stop");
			} else {
				CHECK(wait_hash(h20, 8000), "Mod+Shift+b did not show the section with a value");
			}
		}
	}
#endif

	/* terminating the sampler removes its value; dwm shows "-" and keeps working */
	kill(sampler_pid, SIGTERM);
	CHECK(waitpid(sampler_pid, &status, 0) == sampler_pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "dwmsysmon did not exit cleanly on SIGTERM");
	sampler_pid = 0;
	CHECK(wait_hash(h_dash, 2000), "dwm kept showing a value after the sampler exited");
	responsive("the sampler exiting");

	/* leave the shared test dwm as found */
	set_resources("dwm.showsysmon: 0\n");
	CHECK(wait_hash(h_off, 3000), "the section was not hidden at the end");
	if (sampler_pid > 0)
		kill(sampler_pid, SIGKILL);
	XCloseDisplay(dpy);
	if (failures)
		return EXIT_FAILURE;
	puts("system monitor integration checks passed");
	return EXIT_SUCCESS;
}
