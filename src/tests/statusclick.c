/*
 * Clicks on the status text of the production dwm reach dwmblocks.
 *
 * Run as "statusclick fake <log>" under the name "dwmblocks", it stands in for
 * the real program: it logs the button and block of every SIGUSR1 queued to
 * it. Run as "statusclick <log>", it sets the status through the root window
 * name, clicks the bar with XTEST and checks what the fake received.
 */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

static int failures;

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

static int
fake_dwmblocks(const char *log)
{
	sigset_t set;
	siginfo_t info;
	FILE *fp;

	/* block first so a click can never hit the default action of SIGUSR1 */
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	sigprocmask(SIG_BLOCK, &set, NULL);
	if (!(fp = fopen(log, "w")))
		return EXIT_FAILURE;
	fputs("ready\n", fp);
	fflush(fp);
	while (sigwaitinfo(&set, &info) != -1) {
		fprintf(fp, "button=%d block=%d\n", info.si_value.sival_int & 0xff,
		        (info.si_value.sival_int >> 8) & 0xff);
		fflush(fp);
	}
	return EXIT_FAILURE;
}

/* number of lines the fake wrote, and a copy of the last one */
static int
log_lines(const char *log, char *last, size_t size)
{
	char line[64];
	int count = 0;
	FILE *fp = fopen(log, "r");

	if (!fp)
		return 0;
	last[0] = '\0';
	while (fgets(line, sizeof line, fp)) {
		count++;
		snprintf(last, size, "%s", line);
	}
	fclose(fp);
	last[strcspn(last, "\n")] = '\0';
	return count;
}

/* click button 1 at x on the bar and return what dwmblocks received, or "nothing" */
static const char *
click_status(Display *dpy, int screen, const char *log, int x, char *last, size_t size)
{
	char before[64];
	int count = log_lines(log, before, sizeof before), i;

	XTestFakeMotionEvent(dpy, screen, x, 2, CurrentTime);
	XTestFakeButtonEvent(dpy, Button1, True, CurrentTime);
	XTestFakeButtonEvent(dpy, Button1, False, CurrentTime);
	XFlush(dpy);
	for (i = 0; i < 80; i++) {
		if (log_lines(log, last, size) > count)
			return last;
		pause_millis(25);
	}
	return "nothing";
}

static void
check_click(Display *dpy, int screen, const char *log, const char *what, int x, const char *want)
{
	char last[64];
	const char *got = click_status(dpy, screen, log, x, last, sizeof last);

	CHECK(!strcmp(got, want), "%s: click at x=%d gave \"%s\" (expected \"%s\")", what, x, got, want);
}

static void
set_status(Display *dpy, Window root, const char *text)
{
	XStoreName(dpy, root, text);
	XSync(dpy, False);
	pause_millis(100);
}

int
main(int argc, char *argv[])
{
	Display *dpy;
	Window root;
	int screen, width, event_base, error_base, major, minor;
	const char *log;

	if (argc == 3 && !strcmp(argv[1], "fake"))
		return fake_dwmblocks(argv[2]);
	if (argc != 2) {
		fprintf(stderr, "usage: statusclick <log> | statusclick fake <log>\n");
		return EXIT_FAILURE;
	}
	log = argv[1];
	if (!(dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, "could not open the private X display\n");
		return EXIT_FAILURE;
	}
	if (!XTestQueryExtension(dpy, &event_base, &error_base, &major, &minor)) {
		fprintf(stderr, "status click checks require the XTEST server extension\n");
		return EXIT_FAILURE;
	}
	root = DefaultRootWindow(dpy);
	screen = DefaultScreen(dpy);
	width = DisplayWidth(dpy, screen);

	/*
	 * Codes take no width, so the spacers set the block edges exactly. The
	 * status is drawn from the bar width minus its width and a 2px right
	 * padding: the blocks below end at width - 102 and width - 2.
	 */
	set_status(dpy, root, "\x01^f100^\x02^f100^");
	check_click(dpy, screen, log, "narrow status, first pixel of block 1", width - 202, "button=1 block=1");
	check_click(dpy, screen, log, "narrow status, last pixel of block 1", width - 103, "button=1 block=1");
	check_click(dpy, screen, log, "narrow status, first pixel of block 2", width - 102, "button=1 block=2");
	check_click(dpy, screen, log, "narrow status, last pixel of block 2", width - 3, "button=1 block=2");

	/* wider than the bar: starts at -202, block 1 ends at 798 and block 2 at 1198 */
	set_status(dpy, root, "\x01^f1000^\x02^f400^");
	check_click(dpy, screen, log, "status wider than the bar, block 1", width - 500, "button=1 block=1");
	check_click(dpy, screen, log, "status wider than the bar, last pixel of block 1", width - 403, "button=1 block=1");
	check_click(dpy, screen, log, "status wider than the bar, first pixel of block 2", width - 402, "button=1 block=2");
	check_click(dpy, screen, log, "status wider than the bar, block 2", width - 20, "button=1 block=2");

	set_status(dpy, root, "dwm-test");
	XCloseDisplay(dpy);
	if (failures)
		return EXIT_FAILURE;
	puts("status click regression checks passed");
	return EXIT_SUCCESS;
}
