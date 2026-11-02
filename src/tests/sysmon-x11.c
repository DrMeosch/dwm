/*
 * End-to-end checks of the system monitor section against the running test dwm:
 * a stand-in sampler (this program) and the real dwmsysmon publish through the
 * root window property, and the bar must show the values, "-" when the CPU
 * usage is missing, malformed or stale, and no temperature, fans, GPU usage or
 * graph when they are, while dwm stays responsive. The section is recognised by
 * the bar pixels it produces. The real sampler's graph follows its history, so
 * the checks of its values leave the graph's columns out.
 *
 * usage: sysmon-x11 <dwm pid> <dwmsysmon> <scratch directory> <link>
 *
 * dwmsysmon must be built with SYSMON_STAT_PATH set to <scratch directory>/stat
 * and SYSMON_SYS_PATH set to <scratch directory>/sys. <link> is the symbolic
 * link the test dwm finds as "dwmsysmon" first on its PATH; it must point to
 * <dwmsysmon> when dwm starts.
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#ifdef HAVE_XTEST
#include <X11/extensions/XTest.h>
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STALE_MS 1500 /* dwm.sysmonstale used with the stand-in sampler */
#define SAMPLE_MS 4000 /* how often dwmsysmon samples: SYSMONINTERVAL in config.h */
#define SAMPLER_STALE_MS (2 * SAMPLE_MS + 1000) /* dwm.sysmonstale used with dwmsysmon */
#define VALUE_MS (2 * SAMPLE_MS + 2000) /* until a change of the fixture shows: two samples and a margin */
#define SENSOR_MS (2 * VALUE_MS) /* until a sensor change shows: sensors are read at most every 6000 ms */
#define GRAPH_BARS 16 /* SYSMON_HIST samples in the graph */
#define GRAPH_PX 32 /* sysmongraph in config.h: a bar of 2px each */

static Display *dpy;
static Window root;
static Atom sysmonatom, enabledatom;
static pid_t dwm_pid, sampler_pid;
static int failures;
static int fixture_busy = 20; /* percent busy in the stat file the stand-in kernel writes */
static unsigned long long fixture_total, fixture_idle;
static char fixture_path[512];
static char sys_path[512]; /* the fake sysfs tree dwmsysmon reads its sensors from */
static int fixture_frozen; /* leave the stat file alone, e.g. while it is deliberately broken */
static char sampler_exe[PATH_MAX]; /* the real dwmsysmon, as /proc names its executable */
static char sampler_link[PATH_MAX]; /* what dwm starts as "dwmsysmon" */
static int skip_left = 1, skip_right = 0; /* columns bar_hash leaves out, none while left > right */

/* What the bar shows outside the columns from left to right. */
typedef struct {
	unsigned long hash;
	int left, right;
} Look;

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

/* Write an attribute of the fake sysfs tree, replaced atomically; NULL removes it. */
static void
put_sys(const char *rel, const char *text)
{
	char path[1024], tmp[1100], *p;
	FILE *fp;

	snprintf(path, sizeof path, "%s/%s", sys_path, rel);
	for (p = path; (p = strchr(p + 1, '/'));) {
		*p = '\0';
		mkdir(path, 0755);
		*p = '/';
	}
	if (!text) {
		unlink(path);
		return;
	}
	snprintf(tmp, sizeof tmp, "%s.new", path);
	if (!(fp = fopen(tmp, "w"))) {
		CHECK(0, "cannot write %s", tmp);
		return;
	}
	fputs(text, fp);
	fclose(fp);
	rename(tmp, path);
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
			if (x < skip_left || x > skip_right)
				h = (h ^ XGetPixel(image, x, y)) * 1099511628211UL;
	XDestroyImage(image);
	return h;
}

static XImage *
bar_copy(void)
{
	Window bar = find_bar();
	XWindowAttributes wa;

	if (bar == None || !XGetWindowAttributes(dpy, bar, &wa))
		return NULL;
	return XGetImage(dpy, bar, 0, 0, wa.width, wa.height, AllPlanes, ZPixmap);
}

/* How many pixels differ between two bar images, and the leftmost and rightmost column that does. */
static long
diff_extent(XImage *a, XImage *b, int *left, int *right)
{
	long n = 0;
	int x, y;

	*left = a->width;
	*right = -1;
	for (x = 0; x < a->width && x < b->width; x++)
		for (y = 0; y < a->height && y < b->height; y++)
			if (XGetPixel(a, x, y) != XGetPixel(b, x, y)) {
				n++;
				if (x < *left)
					*left = x;
				*right = x;
			}
	return n;
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

	if (XGetWindowProperty(dpy, root, sysmonatom, 0, 64, False, XA_STRING, &type,
	    &format, &n, &after, &data) == Success && data) {
		if (type == XA_STRING && format == 8 && !after && n < size) {
			memcpy(buf, data, n);
			buf[n] = '\0';
			ok = 1;
		}
		XFree(data);
	}
	return ok;
}

/* Take the history field name (as "cpuhist=") out of buf: its samples, oldest first, and how many. */
static int
split_history(char *buf, const char *name, int *samples)
{
	char *p, *q, *end;
	int n = 0;

	if (!(p = strstr(buf, name)))
		return 0;
	for (q = p + strlen(name); n < GRAPH_BARS + 1; q = end + 1) {
		samples[n++] = (int)strtol(q, &end, 10);
		if (*end != ',')
			break;
	}
	end = p + strcspn(p, " ");
	if (p > buf)
		memmove(p - 1, end, strlen(end) + 1); /* with the space before it */
	else
		memmove(p, end + strspn(end, " "), strlen(end + strspn(end, " ")) + 1);
	return n;
}

/* Wait for the sampler's value, without its histories, to be want; NULL waits for none. */
static int
wait_value(const char *want, long millis)
{
	char buf[300];
	int samples[GRAPH_BARS + 1];
	long waited;

	for (waited = 0; waited <= millis; waited += 25) {
		if (want ? (read_value(buf, sizeof buf) && (split_history(buf, "cpuhist=", samples),
		            split_history(buf, "gpuhist=", samples), !strcmp(buf, want)))
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

/* The interval dwm publishes for the sampler, -1 if none. */
static long
published_interval(void)
{
	Atom type;
	int format;
	unsigned long n, after;
	unsigned char *data = NULL;
	long ms = -1;

	if (XGetWindowProperty(dpy, root, enabledatom, 0, 3, False, XA_CARDINAL, &type,
	    &format, &n, &after, &data) == Success && data) {
		if (type == XA_CARDINAL && format == 32 && n == 3)
			ms = (long)((unsigned long *)data)[2];
		XFree(data);
	}
	return ms;
}

/* Publish another interval in dwm's place, keeping its flag and window. */
static void
set_interval(unsigned long ms)
{
	Atom type;
	int format;
	unsigned long n, after, flag[3];
	unsigned char *data = NULL;

	if (XGetWindowProperty(dpy, root, enabledatom, 0, 3, False, XA_CARDINAL, &type,
	    &format, &n, &after, &data) != Success || !data || type != XA_CARDINAL
	|| format != 32 || n != 3) {
		CHECK(0, "dwm published no interval to replace");
		if (data)
			XFree(data);
		return;
	}
	memcpy(flag, data, sizeof flag);
	XFree(data);
	flag[2] = ms;
	XChangeProperty(dpy, root, enabledatom, XA_CARDINAL, 32, PropModeReplace,
	                (unsigned char *)flag, 3);
	XFlush(dpy);
}

/* ms between two consecutive publishes of the sampler by the X server's clock, -1 if they did not come. */
static long
publish_period(void)
{
	XEvent ev;
	Time first = 0;
	long waited, period = -1;
	int seen = 0;

	XSelectInput(dpy, root, PropertyChangeMask);
	for (waited = 0; waited <= 3 * SAMPLE_MS && period < 0; waited += 25) {
		while (period < 0 && XPending(dpy)) {
			XNextEvent(dpy, &ev);
			if (ev.type != PropertyNotify || ev.xproperty.atom != sysmonatom)
				continue;
			if (seen++)
				period = (long)(ev.xproperty.time - first);
			first = ev.xproperty.time;
		}
		if (period < 0)
			pump(25);
	}
	XSelectInput(dpy, root, NoEventMask);
	XSync(dpy, True);
	return period;
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

/* Make dwm start target as "dwmsysmon" from now on. */
static void
provide_sampler(const char *target)
{
	char tmp[PATH_MAX + 8];

	snprintf(tmp, sizeof tmp, "%s.new", sampler_link);
	unlink(tmp);
	CHECK(symlink(target, tmp) == 0 && rename(tmp, sampler_link) == 0,
	      "cannot make dwm start %s", target);
}

/*
 * How many real samplers run on this X display, whoever started them, and the
 * pid of one of them. A sampler of another display, or one that has exited
 * and is only waiting to be reaped, does not count.
 */
static int
samplers(pid_t *pid)
{
	char path[300], exe[PATH_MAX], env[16384], want[128], *p; /* path: "/proc/", a name of up to 255 bytes and more */
	struct dirent *e;
	DIR *dir;
	ssize_t len;
	int fd, n = 0;

	snprintf(want, sizeof want, "DISPLAY=%s", getenv("DISPLAY"));
	if (!(dir = opendir("/proc")))
		return -1;
	while ((e = readdir(dir))) {
		if (!isdigit((unsigned char)e->d_name[0]))
			continue;
		snprintf(path, sizeof path, "/proc/%s/exe", e->d_name);
		if ((len = readlink(path, exe, sizeof exe - 1)) < 0)
			continue;
		exe[len] = '\0';
		if (strcmp(exe, sampler_exe))
			continue;
		snprintf(path, sizeof path, "/proc/%s/environ", e->d_name);
		if ((fd = open(path, O_RDONLY)) < 0)
			continue;
		len = read(fd, env, sizeof env - 1);
		close(fd);
		if (len <= 0)
			continue;
		env[len] = '\0';
		for (p = env; p < env + len; p += strlen(p) + 1)
			if (!strcmp(p, want)) {
				if (pid)
					*pid = (pid_t)atol(e->d_name);
				n++;
				break;
			}
	}
	closedir(dir);
	return n;
}

/* Wait until pid is the only sampler on this X display. */
static int
wait_only(pid_t pid, long millis)
{
	long waited;
	pid_t found;

	for (waited = 0; waited <= millis; waited += 25) {
		if (samplers(&found) == 1 && found == pid)
			return 1;
		pump(25);
	}
	return 0;
}

static int
wait_samplers(int want, long millis, pid_t *pid)
{
	long waited;

	for (waited = 0; waited <= millis; waited += 25) {
		if (samplers(pid) == want)
			return 1;
		pump(25);
	}
	return 0;
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

/* The bar for payload, as the stand-in sampler publishes it. */
static XImage *
look_at(const char *payload)
{
	reference(payload);
	return bar_copy();
}

/* payload with a field of count samples, oldest first, all worth value but the last, worth last. */
static const char *
with_samples(const char *payload, const char *name, int count, int value, int last)
{
	static char buf[300];
	int i;

	snprintf(buf, sizeof buf, "%s%s%s=", payload, *payload ? " " : "", name);
	for (i = 0; i < count; i++)
		snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "%s%d", i ? "," : "", i == count - 1 ? last : value);
	return buf;
}

/* payload with a CPU usage history of count samples, oldest first, all worth value but the last, worth last. */
static const char *
with_history(const char *payload, int count, int value, int last)
{
	return with_samples(payload, "cpuhist", count, value, last);
}

/* The columns of the graph in the section drawn for payload: those a full history changes. */
static void
graph_columns(const char *payload, int *left, int *right)
{
	XImage *none = look_at(payload), *full = look_at(with_history(payload, GRAPH_BARS, 100, 100));

	*left = 1;
	*right = 0;
	CHECK(none && full && diff_extent(none, full, left, right) > 0, "\"%s\" has no graph", payload);
	if (none)
		XDestroyImage(none);
	if (full)
		XDestroyImage(full);
}

/* What the bar shows for payload (NULL: none) but in the graph's columns, placed as for layout. */
static Look
look_without_graph(const char *layout, const char *payload)
{
	Look l;

	graph_columns(layout, &l.left, &l.right);
	skip_left = l.left;
	skip_right = l.right;
	l.hash = reference(payload);
	skip_left = 1;
	skip_right = 0;
	return l;
}

/* Wait until the bar shows what l describes; bar_hash leaves the same columns out until the next look. */
static int
wait_look(Look l, long millis)
{
	skip_left = l.left;
	skip_right = l.right;
	return wait_hash(l.hash, millis);
}

/* Whether column x of a bar image has anything but its background, which text and bars leave in its top row. */
static int
ink(XImage *img, int x)
{
	int y;

	for (y = 1; y < img->height; y++)
		if (XGetPixel(img, x, y) != XGetPixel(img, x, 0))
			return 1;
	return 0;
}

/* How many pixels of column x differ between a and b, and the lowest row that does. */
static int
column_diff(XImage *a, XImage *b, int x, int *bottom)
{
	int y, n = 0;

	*bottom = -1;
	for (y = 0; y < a->height; y++)
		if (XGetPixel(a, x, y) != XGetPixel(b, x, y)) {
			n++;
			*bottom = y;
		}
	return n;
}

/* The runs of adjacent columns that differ between a and b, at most max of them: where each starts and how wide it is. */
static int
diff_runs(XImage *a, XImage *b, int *at, int *width, int max)
{
	int x, bottom, n = 0, in = 0;

	for (x = 0; x < a->width && x < b->width; x++) {
		if (!column_diff(a, b, x, &bottom)) {
			in = 0;
			continue;
		}
		if (!in && n++ < max) {
			at[n - 1] = x;
			width[n - 1] = 0;
		}
		if (n <= max)
			width[n - 1]++;
		in = 1;
	}
	return n;
}

/*
 * Hold the real sampler and check that dwm draws what it published, graphs
 * included, exactly as it draws the same payload from the stand-in; buf gets
 * the payload.
 */
static int
drawn_as_published(char *buf, size_t size, const char *what)
{
	unsigned long drawn;
	int ok;

	kill(sampler_pid, SIGSTOP);
	pump(300);
	ok = read_value(buf, size);
	skip_left = 1;
	skip_right = 0;
	drawn = steady_hash();
	CHECK(ok && drawn && reference(buf) == drawn, "dwm did not draw %s (\"%s\") as it draws the same payload",
	      what, ok ? buf : "");
	kill(sampler_pid, SIGCONT);
	return ok;
}

int
main(int argc, char *argv[])
{
	unsigned long h_off, h_dash, h20, h80, h42, h_two, h_one, h_nofan, h_temp47;
	Look l_off, l_dash, l20, l80, l_two, l_one, l_nofan, l_temp47;
	char value[300];
	int samples[GRAPH_BARS + 1], nsamples;
	char big[4096], medium[64], huge[200], stub[PATH_MAX], res[64];
	XImage *off_plain, *off_medium, *off_huge, *on_img;
	long n;
	int left, right, width;
	unsigned long wrongtype[1] = { 42 };
	unsigned short wide[8] = { 'c', 'p', 'u', '=', '4', '2' };
	int i, status;
	pid_t pid, stuck;
	FILE *fp;
	static const struct { const char *text; int len; } garbage[] = {
		{ "cpu=abc", 7 }, { "cpu=101", 7 }, { "cpu=-5", 6 }, { "cpu=", 4 }, { "cpu", 3 },
		{ "junk", 4 }, { "\377\377\377\377", 4 }, { "cpu=42x", 7 }, { "cpu=1000000", 11 },
		{ "cpu=42\0cpu=43", 13 }, { "", 0 }, { "\033[2J\033]0;x\007", 10 },
	};

	if (argc != 5)
		return 2;
	dwm_pid = (pid_t)atol(argv[1]);
	snprintf(fixture_path, sizeof fixture_path, "%s/stat", argv[3]);
	snprintf(sys_path, sizeof sys_path, "%s/sys", argv[3]);
	snprintf(sampler_link, sizeof sampler_link, "%s", argv[4]);
	if (!realpath(argv[2], sampler_exe)) {
		perror("sysmon-x11: dwmsysmon");
		return 1;
	}
	/* what dwm starts while this program stands in for the sampler: it exits at once */
	snprintf(stub, sizeof stub, "%s/stub-sampler", argv[3]);
	if (!(fp = fopen(stub, "w")) || fputs("#!/bin/sh\nexit 0\n", fp) < 0 || fclose(fp) || chmod(stub, 0755)) {
		perror("sysmon-x11: stub sampler");
		return 1;
	}
	if (!(dpy = XOpenDisplay(NULL))) {
		fputs("sysmon-x11: cannot open display\n", stderr);
		return 1;
	}
	root = DefaultRootWindow(dpy);
	width = DisplayWidth(dpy, DefaultScreen(dpy));
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
	CHECK(samplers(NULL) == 0, "dwm started a sampler although the section is off by default");
	publish("cpu=42");
	pump(300);
	CHECK(bar_hash() == h_off, "a value published while the section is hidden changed the bar");
	remove_value();

	/* the bar without the section, with a short, a medium and a very long status text */
	memset(medium, 'm', 40);
	medium[40] = '\0';
	memset(huge, 'h', 120);
	huge[120] = '\0';
	off_plain = bar_copy();
	set_status(medium);
	steady_hash();
	off_medium = bar_copy();
	set_status(huge);
	steady_hash();
	off_huge = bar_copy();
	set_status("sysmon-base");
	CHECK(off_plain && off_medium && off_huge && steady_hash() == h_off, "cannot capture the bar");

	/* shown by the resource: the empty section is "-" and takes room from the title */
	provide_sampler(stub);
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
	CHECK(reference("cpu=42 vram=9 something=else") == h42, "unknown fields in the payload changed the value");

	/*
	 * The temperature and every fan are shown when they are there, and a
	 * malformed one is shown as missing without affecting the others.
	 */
	{
		unsigned long full = reference("cpu=42 temp=55 fan=2400,2350");
		unsigned long nofan = reference("cpu=42 temp=55");
		unsigned long notemp = reference("cpu=42 fan=2400,2350");
		unsigned long onefan = reference("cpu=42 temp=55 fan=2400");
		unsigned long nocpu = reference("temp=55 fan=2400,2350");

		CHECK(full && nofan && notemp && onefan && nocpu && full != h42 && full != nofan && full != notemp
		      && full != onefan && nofan != h42 && notemp != h42 && onefan != nofan && nocpu != full
		      && nocpu != h_dash, "the temperature and fans do not look distinct on the bar");
		CHECK(reference("cpu=42 temp=55 fan=2400,2351") != full, "a change of the second fan did not show");
		CHECK(reference("cpu=42 temp=999 fan=2400,2350") == notemp, "a malformed temperature was not missing");
		CHECK(reference("cpu=42 temp=abc fan=2400,2350") == notemp, "a non-numeric temperature was not missing");
		CHECK(reference("cpu=42 temp=55 fan=2400,,1") == nofan, "a malformed fan list was not missing");
		CHECK(reference("cpu=42 temp=55 fan=99999") == nofan, "an implausible fan was not missing");
		CHECK(reference("cpu=42 temp=55 fan=1,2,3,4,5") == nofan, "too many fans were not missing");
		CHECK(reference("cpu=42 temp=55 temp=56 fan=2400,2350") == notemp, "a repeated temperature was not missing");
		CHECK(reference("cpu=4x2 temp=55 fan=2400,2350") == nocpu, "a malformed CPU usage hid the sensors");
		CHECK(reference("cpu=42 temp=55 fan=2400,2350 vram=7") == full, "an unknown field changed the sensors");
		responsive("sensor fields");
	}
	h_two = reference("cpu=20 temp=48 fan=2100,2050");
	h_one = reference("cpu=20 temp=48 fan=2100");
	h_nofan = reference("cpu=20 temp=48");
	h_temp47 = reference("cpu=20 temp=47");
	CHECK(h_two && h_one && h_nofan && h_temp47 && h_two != h_one && h_one != h_nofan && h_nofan != h20
	      && h_temp47 != h_nofan, "the sampler's sensor values do not look distinct on the bar");

	/*
	 * The section sits at the centre of the bar, whatever the status text takes
	 * on the right, and is left out rather than overlapping it when that
	 * does not fit. "cpu=100" with a full graph fills the whole box, so its
	 * ink spans it.
	 */
	publish(with_history("cpu=100", GRAPH_BARS, 100, 100));
	set_status("sysmon-base");
	pump(300);
	steady_hash();
	on_img = bar_copy();
	n = on_img && off_plain ? diff_extent(on_img, off_plain, &left, &right) : 0;
	CHECK(n > 0, "the section left no pixels in the bar");
	CHECK(n > 0 && left > width / 2 - 250 && right < width / 2 + 250,
	      "the section is not near the middle of the bar: columns %d-%d of %d", left, right, width);
	CHECK(n > 0 && (left + right) / 2 >= width / 2 - 8 && (left + right) / 2 <= width / 2 + 8,
	      "the section is not centred: columns %d-%d of %d", left, right, width);
	if (on_img)
		XDestroyImage(on_img);
	set_status(medium);
	pump(300);
	steady_hash();
	on_img = bar_copy();
	n = on_img && off_medium ? diff_extent(on_img, off_medium, &left, &right) : 0;
	CHECK(n > 0 && (left + right) / 2 >= width / 2 - 8 && (left + right) / 2 <= width / 2 + 8,
	      "the section moved with a long status text: columns %d-%d", left, right);
	if (on_img)
		XDestroyImage(on_img);
	set_status(huge);
	pump(300);
	steady_hash();
	on_img = bar_copy();
	n = on_img && off_huge ? diff_extent(on_img, off_huge, &left, &right) : -1;
	CHECK(n == 0, "the section overlapped a status text too long to leave it room (%ld pixels differ)", n);
	if (on_img)
		XDestroyImage(on_img);
	/* the same with the temperature and two fans: a wider section, still centred and still left out */
	{
		static const char *payloads[] = {
			"cpu=100 temp=100 fan=9999", "cpu=100 temp=100 fan=9999,9999",
			"cpu=100 temp=100 fan=9999,9999,9999,9999",
			"cpu=100 temp=100 fan=9999,9999,9999,9999 gpu=100",
		};
		char filled[300];
		int lastwidth = 0;

		for (i = 0; i < (int)(sizeof payloads / sizeof *payloads); i++) {
			/* full graphs, so the ink spans the whole box */
			snprintf(filled, sizeof filled, "%s", with_history(payloads[i], GRAPH_BARS, 100, 100));
			if (strstr(filled, "gpu="))
				snprintf(filled, sizeof filled, "%s", with_samples(filled, "gpuhist", GRAPH_BARS, 100, 100));
			publish(filled);
			set_status("sysmon-base");
			pump(300);
			steady_hash();
			on_img = bar_copy();
			n = on_img && off_plain ? diff_extent(on_img, off_plain, &left, &right) : 0;
			CHECK(n > 0 && (left + right) / 2 >= width / 2 - 8 && (left + right) / 2 <= width / 2 + 8,
			      "\"%s\" is not centred: columns %d-%d of %d", payloads[i], left, right, width);
			CHECK(n > 0 && right - left > lastwidth, "\"%s\" is not wider than the section with fewer fields (%d, %d)",
			      payloads[i], right - left, lastwidth);
			lastwidth = right - left;
			if (on_img)
				XDestroyImage(on_img);
			set_status(medium);
			pump(300);
			steady_hash();
			on_img = bar_copy();
			n = on_img && off_medium ? diff_extent(on_img, off_medium, &left, &right) : 0;
			CHECK(n > 0 && (left + right) / 2 >= width / 2 - 8 && (left + right) / 2 <= width / 2 + 8,
			      "\"%s\" moved with a long status text: columns %d-%d", payloads[i], left, right);
			if (on_img)
				XDestroyImage(on_img);
			set_status(huge);
			pump(300);
			steady_hash();
			on_img = bar_copy();
			n = on_img && off_huge ? diff_extent(on_img, off_huge, &left, &right) : -1;
			CHECK(n == 0, "\"%s\" overlapped a status text too long to leave it room (%ld pixels differ)",
			      payloads[i], n);
			if (on_img)
				XDestroyImage(on_img);
		}
	}

	/*
	 * The graph of the CPU usage history: 16 bars of 2px between the CPU usage
	 * and the other fields, which keep their pixels. A sample is a bar rising
	 * from the same bottom row, as high as its share of a full bar and at least
	 * a pixel; the newest is on the right, and a short history fills the right
	 * of the graph only. A malformed history draws nothing.
	 */
	{
		static const char *fields = "cpu=42 temp=55 fan=2400,2350";
		static const char *bad[] = {
			"cpuhist=", "cpuhist=101", "cpuhist=1,,2", "cpuhist=4x", "cpuhist=-1", "cpuhist=1,", "cpuhist=0042",
			"cpuhist=1 cpuhist=2", "CPUHIST=1", "cpuhist=1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17",
		};
		char payload[256];
		XImage *none, *img;
		int gl, gr, x, h, bottom, full = 0, fullbottom = -1, ok;
		unsigned long h_fields;

		set_status("sysmon-base");
		none = look_at(fields);
		h_fields = steady_hash();
		graph_columns(fields, &gl, &gr);
		CHECK(gr - gl + 1 == GRAPH_PX, "the graph is %d px wide, not %d", gr - gl + 1, GRAPH_PX);
		n = none && off_plain ? diff_extent(none, off_plain, &left, &right) : 0;
		CHECK(n > 0 && left < gl - 8 && right > gr + 8,
		      "the graph (columns %d-%d) is not between the CPU usage and the other fields (%d-%d)", gl, gr, left, right);
		for (x = gl - 4, ok = 1; none && off_plain && x <= gr + 4; x++)
			ok = ok && column_diff(none, off_plain, x, &bottom) == 0;
		CHECK(ok, "text of the section is drawn in or next to the graph's columns %d-%d", gl, gr);

		if ((img = look_at(with_history(fields, GRAPH_BARS, 100, 100)))) {
			for (x = gl, ok = 1; none && x <= gr; x++) {
				h = column_diff(img, none, x, &bottom);
				if (x == gl) {
					full = h;
					fullbottom = bottom;
				}
				ok = ok && h == full && bottom == fullbottom;
			}
			CHECK(ok && full >= 6 && full < img->height - 2, "full bars are not all %d px high on one row", full);
			XDestroyImage(img);
		}
		if ((img = look_at(with_history(fields, 2, 100, 0)))) {
			for (x = gl, ok = 1; none && x <= gr; x++) {
				h = column_diff(img, none, x, &bottom);
				if (x >= gr - 1)
					ok = ok && h == 1 && bottom == fullbottom;
				else if (x >= gr - 3)
					ok = ok && h == full && bottom == fullbottom;
				else
					ok = ok && h == 0;
			}
			CHECK(ok, "a history of 100%% then 0%% is not a full bar left of a line at the right of the graph");
			XDestroyImage(img);
		}
		if ((img = look_at(with_history(fields, 1, 50, 50)))) {
			for (x = gl, ok = 1; none && x <= gr; x++) {
				h = column_diff(img, none, x, &bottom);
				ok = ok && h == (x >= gr - 1 ? (50 * full + 50) / 100 : 0) && (!h || bottom == fullbottom);
			}
			CHECK(ok, "a single sample of 50%% is not a half bar at the right of the graph");
			XDestroyImage(img);
		}
		if ((img = look_at(with_history(fields, 1, 99, 99)))) { /* rounded to the nearest pixel */
			h = none ? column_diff(img, none, gr, &bottom) : -1;
			CHECK(h == full, "a sample of 99%% is %d px high, not a full bar of %d", h, full);
			XDestroyImage(img);
		}
		for (i = 0; i < (int)(sizeof bad / sizeof *bad); i++) {
			snprintf(payload, sizeof payload, "%s %s", fields, bad[i]);
			CHECK(reference(payload) == h_fields, "the malformed history \"%s\" changed the bar", bad[i]);
		}
		CHECK(reference(with_history(fields, 3, 20, 30)) != h_fields, "a valid history did not show after malformed ones");
		CHECK(reference("cpu=42 temp=55 cpuhist=4,,2 fan=2400,2350") == h_fields, "a malformed history moved the fields");
		if (none)
			XDestroyImage(none);

		/*
		 * The GPU usage and its graph come last, after the fans: a graph as
		 * wide as the CPU's, with bars as high on the same row, that leaves the
		 * CPU's alone. A malformed GPU usage or history is left out on its own.
		 */
		{
			static const char *badgpu[] = {
				"gpu=", "gpu=101", "gpu=-1", "gpu=4x", "gpu=0042", "gpu=7 gpu=7", "GPU=7", "gpu=7,8",
			};
			static const char *badhist[] = {
				"gpuhist=", "gpuhist=101", "gpuhist=1,,2", "gpuhist=-1", "gpuhist=0042", "gpuhist=1 gpuhist=2",
				"gpuhist=1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17",
			};
			char gfields[128];
			XImage *g, *gfull;
			int ggl = 1, ggr = 0, cgl = 1, cgr = 0, between;
			unsigned long h_gpu, h_gpu9;

			snprintf(gfields, sizeof gfields, "%s gpu=7", fields);
			h_gpu = reference(gfields);
			h_gpu9 = reference("cpu=42 temp=55 fan=2400,2350 gpu=9");
			CHECK(h_gpu && h_gpu9 && h_gpu != h_fields && h_gpu != h_gpu9, "the GPU usage and its value do not show");
			for (i = 0; i < (int)(sizeof badgpu / sizeof *badgpu); i++) {
				snprintf(payload, sizeof payload, "%s %s", fields, badgpu[i]);
				CHECK(reference(payload) == h_fields, "the malformed GPU usage \"%s\" changed the bar", badgpu[i]);
			}
			for (i = 0; i < (int)(sizeof badhist / sizeof *badhist); i++) {
				snprintf(payload, sizeof payload, "%s %s", gfields, badhist[i]);
				CHECK(reference(payload) == h_gpu, "the malformed GPU history \"%s\" changed the bar", badhist[i]);
			}
			CHECK(reference(with_samples(fields, "gpuhist", 3, 20, 30)) != h_fields,
			      "a GPU history without its usage drew nothing");

			graph_columns(gfields, &cgl, &cgr);
			g = look_at(gfields);
			gfull = look_at(with_samples(gfields, "gpuhist", GRAPH_BARS, 100, 100));
			n = g && gfull ? diff_extent(g, gfull, &ggl, &ggr) : 0;
			CHECK(n > 0 && ggr - ggl + 1 == GRAPH_PX, "the GPU graph is %d px wide, not %d", ggr - ggl + 1, GRAPH_PX);
			for (x = cgr + 1, between = 0; g && x < ggl - 4; x++)
				between += ink(g, x);
			CHECK(cgr - cgl + 1 == GRAPH_PX && between > 20,
			      "the GPU graph (columns %d-%d) does not follow the CPU graph (%d-%d) and the other fields", ggl, ggr, cgl, cgr);
			for (x = ggl - 4, ok = 1; g && off_plain && x <= ggr + 4; x++)
				ok = ok && column_diff(g, off_plain, x, &bottom) == 0;
			CHECK(ok, "text of the section is drawn in or next to the GPU graph's columns %d-%d", ggl, ggr);
			for (x = ggl, ok = 1; g && gfull && x <= ggr; x++) {
				h = column_diff(gfull, g, x, &bottom);
				ok = ok && h == full && bottom == fullbottom;
			}
			CHECK(ok, "full GPU bars are not as high as the CPU's (%d px) on the same row", full);
			for (x = cgl, ok = 1; g && gfull && x <= cgr; x++)
				ok = ok && column_diff(gfull, g, x, &bottom) == 0;
			CHECK(ok, "the GPU history changed the CPU graph");
			if ((img = look_at(with_samples(gfields, "gpuhist", 2, 100, 0)))) {
				for (x = ggl, ok = 1; g && x <= ggr; x++) {
					h = column_diff(img, g, x, &bottom);
					if (x >= ggr - 1)
						ok = ok && h == 1 && bottom == fullbottom;
					else if (x >= ggr - 3)
						ok = ok && h == full && bottom == fullbottom;
					else
						ok = ok && h == 0;
				}
				CHECK(ok, "a GPU history of 100%% then 0%% is not a full bar left of a line at the right of its graph");
				XDestroyImage(img);
			}
			if (g)
				XDestroyImage(g);
			if (gfull)
				XDestroyImage(gfull);
		}

		/*
		 * A status text that takes the graph's room cuts it, oldest bars first,
		 * before the section is left out, and it is never drawn over: the
		 * status is a red rectangle of a given width.
		 */
		{
			char status[96];
			int lo = 100, hi = 1000, mid, w = -1, red, y, cpu_end, status_at, last;
			XImage *a = NULL, *b = NULL;

			while (lo <= hi) {
				mid = (lo + hi) / 2;
				snprintf(status, sizeof status, "^c#ff0000^^r0,0,%d,99^^f%d^", mid, mid);
				set_status(status);
				if (a)
					XDestroyImage(a);
				if (b)
					XDestroyImage(b);
				a = look_at(fields);
				b = look_at(with_history(fields, GRAPH_BARS, 100, 100));
				w = a && b && diff_extent(a, b, &gl, &gr) ? gr - gl + 1 : 0;
				if (w == GRAPH_PX)
					lo = mid + 1;
				else if (w == 0)
					hi = mid - 1;
				else
					break;
			}
			CHECK(w > 0 && w < GRAPH_PX && w % 2 == 0, "no status width cut the graph to whole bars (%d px)", w);
			if (w > 0 && w < GRAPH_PX && b) {
				for (x = 0, red = 0; x < b->width; x++)
					for (y = 0; y < b->height; y++)
						red += XGetPixel(b, x, y) == 0xff0000;
				CHECK(red == mid * b->height, "the section with a cut graph drew over the status (%d of %d red pixels)",
				      red, mid * b->height);
				/* and keeps as far from the status as the graph is from the CPU usage */
				for (x = gl - 1, cpu_end = -1; x >= 0 && cpu_end < 0; x--)
					if (ink(b, x))
						cpu_end = x;
				for (x = gr + 1, status_at = -1, last = -1; x < b->width && status_at < 0; x++) {
					if (XGetPixel(b, x, 0) == 0xff0000)
						status_at = x;
					else if (ink(b, x))
						last = x;
				}
				CHECK(cpu_end >= 0 && last > gr && status_at > last && status_at - last >= gl - cpu_end,
				      "the section with a cut graph ends %d px before the status, the graph starts %d px after the CPU usage",
				      status_at - last, gl - cpu_end);
				if (a)
					XDestroyImage(a);
				a = look_at(with_history(fields, GRAPH_BARS, 100, 0));
				for (x = gl, ok = 1; a && x <= gr; x++) {
					h = column_diff(a, b, x, &bottom);
					ok = ok && h == (x >= gr - 1 ? full - 1 : 0);
				}
				CHECK(ok, "the cut graph did not keep the newest samples");
			}
			if (a)
				XDestroyImage(a);
			if (b)
				XDestroyImage(b);
		}

		/*
		 * With a GPU, both graphs give up the same bars, keeping the newest,
		 * and the GPU graph, last in the section, keeps its gap from the status.
		 */
		{
			char status[96], gfields[128], both[300], lastzero[300];
			int lo = 100, hi = 1000, mid, runs = -1, at[3], wd[3], red, y, cpu_end, status_at, last;
			XImage *a = NULL, *b = NULL;

			snprintf(gfields, sizeof gfields, "%s gpu=7", fields);
			snprintf(both, sizeof both, "%s", with_history(gfields, GRAPH_BARS, 100, 100));
			snprintf(lastzero, sizeof lastzero, "%s", with_samples(both, "gpuhist", GRAPH_BARS, 100, 0));
			snprintf(both, sizeof both, "%s", with_samples(both, "gpuhist", GRAPH_BARS, 100, 100));
			while (lo <= hi) {
				mid = (lo + hi) / 2;
				snprintf(status, sizeof status, "^c#ff0000^^r0,0,%d,99^^f%d^", mid, mid);
				set_status(status);
				if (a)
					XDestroyImage(a);
				if (b)
					XDestroyImage(b);
				a = look_at(gfields);
				b = look_at(both);
				runs = a && b ? diff_runs(a, b, at, wd, 3) : 0;
				if (runs == 2 && wd[0] == GRAPH_PX && wd[1] == GRAPH_PX)
					lo = mid + 1;
				else if (runs == 0)
					hi = mid - 1;
				else
					break;
			}
			CHECK(runs == 2 && wd[0] == wd[1] && wd[0] > 0 && wd[0] < GRAPH_PX && wd[0] % 2 == 0,
			      "no status width cut both graphs to the same whole bars (%d runs, %d and %d px)",
			      runs, runs > 0 ? wd[0] : 0, runs > 1 ? wd[1] : 0);
			if (runs == 2 && b) {
				for (x = 0, red = 0; x < b->width; x++)
					for (y = 0; y < b->height; y++)
						red += XGetPixel(b, x, y) == 0xff0000;
				CHECK(red == mid * b->height, "the section with cut graphs drew over the status (%d of %d red pixels)",
				      red, mid * b->height);
				for (x = at[0] - 1, cpu_end = -1; x >= 0 && cpu_end < 0; x--)
					if (ink(b, x))
						cpu_end = x;
				for (x = at[1], status_at = -1, last = -1; x < b->width && status_at < 0; x++) {
					if (XGetPixel(b, x, 0) == 0xff0000)
						status_at = x;
					else if (ink(b, x))
						last = x;
				}
				CHECK(cpu_end >= 0 && last >= at[1] + wd[1] - 1 && status_at > last && status_at - last >= at[0] - cpu_end,
				      "the section with cut graphs ends %d px before the status, the graph starts %d px after the CPU usage",
				      status_at - last, at[0] - cpu_end);
				if (a)
					XDestroyImage(a);
				a = look_at(lastzero);
				for (x = at[1], ok = 1; a && x < at[1] + wd[1]; x++) {
					h = column_diff(a, b, x, &bottom);
					ok = ok && h == (x >= at[1] + wd[1] - 2 ? full - 1 : 0);
				}
				CHECK(ok, "the cut GPU graph did not keep the newest samples");
			}
			if (a)
				XDestroyImage(a);
			if (b)
				XDestroyImage(b);
		}
		set_status("sysmon-base");
	}

	set_status("sysmon-base");
	XDestroyImage(off_plain);
	XDestroyImage(off_medium);
	XDestroyImage(off_huge);
	remove_value();
	CHECK(wait_hash(h_dash, 3000), "the section did not return to \"-\" after the centring checks");

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
	CHECK(reference(with_history("cpu=80", 3, 70, 80)) != h80, "a history did not show before it went stale");
	CHECK(wait_hash(h_dash, STALE_MS + 3000), "a history that was never refreshed did not go stale");
	responsive("a stale history");
	remove_value();
	CHECK(wait_hash(h_dash, 2000), "the value did not go away");

	/*
	 * The real sampler's graph shows whatever history it has gathered, so its
	 * values are checked without the graph's columns, wherever the fields it
	 * has place them.
	 */
	l20 = look_without_graph("cpu=20", "cpu=20");
	l80 = look_without_graph("cpu=20", "cpu=80");
	l_dash = look_without_graph("cpu=20", NULL);
	l_two = look_without_graph("cpu=20 temp=48 fan=2100,2050", "cpu=20 temp=48 fan=2100,2050");
	l_one = look_without_graph("cpu=20 temp=48 fan=2100", "cpu=20 temp=48 fan=2100");
	l_nofan = look_without_graph("cpu=20 temp=48", "cpu=20 temp=48");
	l_temp47 = look_without_graph("cpu=20 temp=48", "cpu=20 temp=47");
	CHECK(l20.hash && l80.hash && l_dash.hash && l20.hash != l80.hash && l20.hash != l_dash.hash
	      && l_two.hash != l_one.hash && l_nofan.hash != l_temp47.hash,
	      "the values do not look distinct without the graph");

	/* the real sampler, started by dwm: values from a stat file, then hang, bad data and no data */
	set_resources("dwm.showsysmon: 0\n");
	CHECK(wait_hash(h_off, 3000), "the section did not hide before the real sampler's turn");
	skip_left = l20.left;
	skip_right = l20.right;
	l_off.hash = steady_hash();
	l_off.left = l20.left;
	l_off.right = l20.right;
	provide_sampler(sampler_exe);
	snprintf(res, sizeof res, "dwm.showsysmon: 1\ndwm.sysmonstale: %d\n", SAMPLER_STALE_MS);
	set_resources(res);
	CHECK(wait_samplers(1, 3000, &sampler_pid), "showing the section did not start dwmsysmon");
	fixture_busy = 20;
	CHECK(wait_look(l20, VALUE_MS), "dwmsysmon's 20%% was not shown by dwm");
	/* no sysfs sensors at all: only the CPU usage is published */
	CHECK(wait_value("cpu=20", SAMPLE_MS + 1000), "dwmsysmon did not publish exactly cpu=20 without sensors");
	fixture_busy = 80;
	CHECK(wait_look(l80, VALUE_MS), "dwmsysmon's 80%% was not shown by dwm");
	fixture_busy = 20;
	CHECK(wait_look(l20, VALUE_MS), "dwmsysmon's change back to 20%% was not shown by dwm");

	/*
	 * The sampler's history holds its samples, oldest first, up to the CPU
	 * usage it publishes, and the graph shows them as the stand-in's would.
	 */
	{
		char raw[300];
		unsigned long drawn;
		int first20 = -1, then80 = -1, ok;

		kill(sampler_pid, SIGSTOP);
		pump(300);
		ok = read_value(raw, sizeof raw);
		snprintf(value, sizeof value, "%s", ok ? raw : "");
		nsamples = split_history(value, "cpuhist=", samples);
		CHECK(ok && !strcmp(value, "cpu=20") && nsamples >= 3 && nsamples <= GRAPH_BARS && samples[nsamples - 1] == 20,
		      "dwmsysmon published \"%s\", not 20%% with its history", raw);
		for (i = 0; i < nsamples; i++) {
			if (samples[i] == 20 && first20 < 0)
				first20 = i;
			if (samples[i] == 80 && first20 >= 0 && then80 < 0)
				then80 = i;
		}
		CHECK(first20 >= 0 && then80 > first20 && then80 < nsamples - 1,
		      "the history \"%s\" does not hold 20%%, then 80%%, then 20%% in order", raw);
		skip_left = 1;
		skip_right = 0;
		drawn = steady_hash();
		CHECK(reference("cpu=20") != drawn, "dwm drew no graph for the sampler's history");
		CHECK(reference(raw) == drawn, "dwm did not draw the sampler's history as it draws the same payload");
		kill(sampler_pid, SIGCONT);
		CHECK(wait_look(l20, VALUE_MS), "the value did not return after the history check");
	}

	/*
	 * The sampler samples as often as dwm says: SYSMONINTERVAL from config.h,
	 * 4000 ms by default, or any other interval in range that dwm publishes,
	 * and its own default for one out of range.
	 */
	CHECK(published_interval() == SAMPLE_MS, "dwm did not tell the sampler to sample every %d ms (%ld)",
	      SAMPLE_MS, published_interval());
	n = publish_period();
	CHECK(n >= SAMPLE_MS - 250 && n <= SAMPLE_MS + 250, "dwmsysmon published every %ld ms, not every %d", n, SAMPLE_MS);
	set_interval(1000);
	n = publish_period();
	CHECK(n >= 750 && n <= 1250, "dwmsysmon published every %ld ms, not every 1000 dwm asked for", n);
	set_interval(100);
	n = publish_period();
	CHECK(n >= SAMPLE_MS - 250 && n <= SAMPLE_MS + 250, "an interval below the minimum gave a period of %ld ms", n);
	set_interval(1000);
	publish_period();
	set_interval(3600000);
	n = publish_period();
	CHECK(n >= SAMPLE_MS - 250 && n <= SAMPLE_MS + 250, "an interval above the maximum gave a period of %ld ms", n);
	set_interval(SAMPLE_MS);
	CHECK(wait_look(l20, VALUE_MS), "the value changed while the interval did");

	kill(sampler_pid, SIGSTOP); /* a hung sampler */
	CHECK(wait_look(l_dash, SAMPLER_STALE_MS + 3000), "a hung sampler's value did not go stale");
	responsive("a hung sampler");
	kill(sampler_pid, SIGCONT);
	CHECK(wait_look(l20, VALUE_MS), "the value did not return after the sampler resumed");

	{ /* unreadable and malformed stat files give "-", and recover */
		FILE *fp = fopen(fixture_path, "w");

		fixture_frozen = 1;
		if (fp) {
			fputs("cpu  not numbers at all\n", fp);
			fclose(fp);
		}
	}
	CHECK(wait_look(l_dash, SAMPLE_MS + 2000), "a malformed stat file was not shown as \"-\"");
	CHECK(wait_value(NULL, 1000), "dwmsysmon kept publishing for a malformed stat file");
	responsive("a malformed stat file");
	unlink(fixture_path);
	pause_millis(1200);
	CHECK(bar_hash() == l_dash.hash, "a missing stat file was not shown as \"-\"");
	CHECK(kill(dwm_pid, 0) == 0, "dwm died with the sampler's data missing");
	fixture_frozen = 0;
	CHECK(wait_look(l20, VALUE_MS), "the value did not return after the stat file came back");

	/* hidden again: the sampler exits and takes its value along; shown again, dwm starts a new one */
	set_resources("dwm.showsysmon: 0\n");
	CHECK(wait_look(l_off, 3000), "turning the section off through Xresources did not restore the bar");
	CHECK(wait_samplers(0, 3000, NULL), "the sampler kept running while the section is hidden");
	CHECK(wait_value(NULL, 3000), "the sampler left its value behind while hidden");
	CHECK(enabled_flag() == 0, "dwm did not tell the sampler the section is hidden again");
	set_resources("dwm.showsysmon: 1\n");
	CHECK(wait_samplers(1, 3000, &pid) && pid != sampler_pid, "showing the section again did not start a new sampler");
	sampler_pid = pid;
	CHECK(wait_look(l20, VALUE_MS), "the value did not return when the section was shown again");

	/*
	 * A sampler that misses the stop, as one stuck in a sensor read would, does
	 * not hold up dwm, and the sampler dwm starts next retires it.
	 */
	stuck = sampler_pid;
	kill(stuck, SIGSTOP);
	set_resources("dwm.showsysmon: 0\n");
	CHECK(wait_look(l_off, 3000), "a stuck sampler held up hiding the section");
	responsive("hiding the section of a stuck sampler");
	set_resources("dwm.showsysmon: 1\n");
	CHECK(wait_samplers(2, 3000, NULL), "showing the section did not start a sampler beside the stuck one");
	CHECK(wait_look(l20, VALUE_MS), "the sampler started beside a stuck one showed no value");
	responsive("starting a sampler beside a stuck one");
	kill(stuck, SIGCONT);
	CHECK(wait_samplers(1, 3000, &pid) && pid != stuck, "the stuck sampler was not retired by the new one");
	sampler_pid = pid;
	CHECK(wait_look(l20, VALUE_MS), "the value did not return from the sampler that replaced a stuck one");

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
				CHECK(wait_look(l_off, 3000), "Mod+Shift+b did not hide the section");
				CHECK(enabled_flag() == 0, "Mod+Shift+b did not tell the sampler to stop");
				CHECK(wait_samplers(0, 3000, NULL), "Mod+Shift+b did not stop the sampler");
				CHECK(wait_value(NULL, 3000), "the sampler stopped by Mod+Shift+b left its value behind");
			} else {
				CHECK(wait_samplers(1, 3000, &sampler_pid), "Mod+Shift+b did not start the sampler again");
				CHECK(wait_look(l20, VALUE_MS), "Mod+Shift+b did not show the section with a value");
			}
		}
	}
#endif

	/*
	 * Sensors from a fake sysfs tree: a Dell-like laptop whose two fans show up
	 * on a fast and a slow chip. Hiding and showing the section starts a new
	 * sampler, which looks for sources at once. Fans that go away are no longer
	 * shown, and so is a malformed temperature.
	 */
	put_sys("devices/hwmon1/name", "coretemp\n");
	put_sys("devices/hwmon1/temp1_input", "48000\n");
	put_sys("devices/hwmon1/temp1_label", "Package id 0\n");
	put_sys("devices/hwmon2/name", "dell_ddv\n");
	put_sys("devices/hwmon2/fan1_input", "2100\n");
	put_sys("devices/hwmon2/fan2_input", "2050\n");
	put_sys("devices/hwmon3/name", "dell_smm\n");
	put_sys("devices/hwmon3/fan1_input", "9000\n");
	put_sys("devices/hwmon3/fan2_input", "9001\n");
	{
		static const char *chips[] = { "hwmon1", "hwmon2", "hwmon3" };
		char link[1024], target[64];

		put_sys("class/hwmon/.keep", "");
		for (i = 0; i < 3; i++) {
			snprintf(link, sizeof link, "%s/class/hwmon/%s", sys_path, chips[i]);
			snprintf(target, sizeof target, "../../devices/%s", chips[i]);
			CHECK(symlink(target, link) == 0, "cannot link %s", link);
		}
	}
	set_resources("dwm.showsysmon: 0\n");
	CHECK(wait_look(l_off, 3000), "the section did not hide before the sensors appeared");
	CHECK(wait_samplers(0, 3000, NULL), "the sampler kept running while the section is hidden");
	set_resources("dwm.showsysmon: 1\n");
	CHECK(wait_samplers(1, 3000, &sampler_pid), "showing the section did not start the sampler");
	CHECK(wait_value("cpu=20 temp=48 fan=2100,2050", VALUE_MS), "dwmsysmon did not publish the temperature and both fans");
	CHECK(wait_look(l_two, 3000), "dwm did not show the temperature and both fans");

	put_sys("devices/hwmon2/fan2_input", NULL);
	CHECK(wait_value("cpu=20 temp=48 fan=2100", SENSOR_MS), "a fan that went away was still published");
	CHECK(wait_look(l_one, 3000), "dwm did not show the one fan left");
	put_sys("devices/hwmon2/fan1_input", NULL);
	CHECK(wait_value("cpu=20 temp=48", SENSOR_MS), "fans that went away were still published");
	CHECK(wait_look(l_nofan, 3000), "dwm did not show the temperature without fans");
	put_sys("devices/hwmon1/temp1_input", "48 degrees\n");
	CHECK(wait_value("cpu=20", SENSOR_MS), "a malformed temperature was published");
	CHECK(wait_look(l20, 3000), "dwm showed a malformed temperature");
	responsive("sensors going away");

	/*
	 * A sensor read that hangs, as slow firmware calls do, holds up only the
	 * sampler: dwm stays responsive and lets the value go stale, and the
	 * value returns when the read completes.
	 */
	{
		char fifo[1024];
		int fd = -1;

		snprintf(fifo, sizeof fifo, "%s/devices/hwmon1/temp1_input", sys_path);
		unlink(fifo);
		CHECK(mkfifo(fifo, 0644) == 0, "cannot make a fifo");
		CHECK(wait_look(l_dash, SENSOR_MS + SAMPLER_STALE_MS), "the value of a sampler stuck in a sensor read did not go stale");
		responsive("a sensor read that hangs");
		CHECK(kill(sampler_pid, 0) == 0, "the sampler died in a slow sensor read");
		for (i = 0; i < 400 && fd < 0; i++) { /* the sampler is the reader the fifo waits for */
			if ((fd = open(fifo, O_WRONLY | O_NONBLOCK)) < 0)
				pump(25);
		}
		CHECK(fd >= 0, "the sampler never opened the slow sensor");
		if (fd >= 0) {
			CHECK(write(fd, "47000\n", 6) == 6, "cannot answer the slow sensor read");
			close(fd);
		}
		CHECK(wait_value("cpu=20 temp=47", VALUE_MS), "the value did not return after the slow sensor read");
		CHECK(wait_look(l_temp47, 3000), "dwm did not show the value after the slow sensor read");
		put_sys("devices/hwmon1/temp1_input", "47000\n"); /* a plain file again, before the next read */
	}

	/*
	 * A GPU in the fake sysfs tree, an amdgpu card: the sampler publishes its
	 * usage and history while it is active, and dwm draws them as it draws the
	 * same payload. Suspended, it is idle, and never read: its usage is then a
	 * FIFO that would hold up a sampler reading it. When the card goes away,
	 * so does the GPU usage.
	 */
	{
		char raw[300], fifo[1024];
		int gsamples[GRAPH_BARS + 1], ng, fd, first37 = -1, then80 = -1;

		/* the slow chip's fans, used by a sampler that looks for sources afresh, would be published too */
		put_sys("devices/hwmon3/fan1_input", NULL);
		put_sys("devices/hwmon3/fan2_input", NULL);
		put_sys("class/drm/card0/device/power/runtime_status", "active\n");
		put_sys("class/drm/card0/device/gpu_busy_percent", "37\n");
		put_sys("class/drm/card0-eDP-1/status", "connected\n");
		set_resources("dwm.showsysmon: 0\n");
		CHECK(wait_samplers(0, 3000, NULL), "the sampler kept running while the section is hidden");
		set_resources("dwm.showsysmon: 1\n");
		CHECK(wait_samplers(1, 3000, &sampler_pid), "showing the section did not start the sampler");
		CHECK(wait_value("cpu=20 temp=47 gpu=37", VALUE_MS), "dwmsysmon did not publish the GPU usage");
		put_sys("class/drm/card0/device/gpu_busy_percent", "80\n");
		CHECK(wait_value("cpu=20 temp=47 gpu=80", VALUE_MS), "dwmsysmon did not publish a change of the GPU usage");
		if (drawn_as_published(raw, sizeof raw, "the GPU usage and its history")) {
			ng = split_history(raw, "gpuhist=", gsamples);
			for (i = 0; i < ng; i++) {
				if (gsamples[i] == 37 && first37 < 0)
					first37 = i;
				if (gsamples[i] == 80 && first37 >= 0 && then80 < 0)
					then80 = i;
			}
			CHECK(first37 >= 0 && then80 > first37 && gsamples[ng - 1] == 80,
			      "the GPU history does not hold 37%% then 80%% up to the usage (%d samples)", ng);
		}
		responsive("the GPU usage");

		/* suspended: idle, with the other values going on, and never read */
		set_resources("dwm.showsysmon: 0\n");
		CHECK(wait_samplers(0, 3000, NULL), "the sampler kept running while the section is hidden");
		snprintf(fifo, sizeof fifo, "%s/class/drm/card0/device/gpu_busy_percent", sys_path);
		put_sys("class/drm/card0/device/power/runtime_status", "suspended\n");
		unlink(fifo);
		CHECK(mkfifo(fifo, 0644) == 0, "cannot make a fifo");
		set_resources("dwm.showsysmon: 1\n");
		CHECK(wait_samplers(1, 3000, &sampler_pid), "showing the section did not start the sampler");
		CHECK(wait_value("cpu=20 temp=47 gpu=0", VALUE_MS), "a suspended GPU was not published as idle");
		fixture_busy = 80;
		CHECK(wait_value("cpu=80 temp=47 gpu=0", VALUE_MS), "the sampler stopped beside a suspended GPU");
		fixture_busy = 20;
		CHECK(wait_value("cpu=20 temp=47 gpu=0", VALUE_MS), "the sampler stopped beside a suspended GPU");
		drawn_as_published(raw, sizeof raw, "a suspended GPU");
		errno = 0;
		fd = open(fifo, O_WRONLY | O_NONBLOCK); /* fails while nobody reads it */
		CHECK(fd < 0 && errno == ENXIO, "the sampler read the usage of a suspended GPU");
		if (fd >= 0)
			close(fd);
		responsive("a suspended GPU");

		/* active again, then gone, as when its driver is unbound: nothing is drawn for it */
		set_resources("dwm.showsysmon: 0\n");
		CHECK(wait_samplers(0, 3000, NULL), "the sampler kept running while the section is hidden");
		unlink(fifo);
		put_sys("class/drm/card0/device/gpu_busy_percent", "12\n");
		put_sys("class/drm/card0/device/power/runtime_status", "active\n");
		/* a read keeps it awake 10 s: it is read only every third sample of 4 s */
		put_sys("class/drm/card0/device/power/autosuspend_delay_ms", "10000\n");
		set_resources("dwm.showsysmon: 1\n");
		CHECK(wait_samplers(1, 3000, &sampler_pid), "showing the section did not start the sampler");
		CHECK(wait_value("cpu=20 temp=47 gpu=12", VALUE_MS), "the GPU usage did not return when it woke up");
		{
			int csamples[GRAPH_BARS + 1], nc = 0;
			long waited;

			ng = 0;
			for (waited = 0; waited <= 9 * SAMPLE_MS && nc < 7; waited += 100) {
				if (read_value(raw, sizeof raw)) {
					nc = split_history(raw, "cpuhist=", csamples);
					ng = split_history(raw, "gpuhist=", gsamples);
				}
				pump(100);
			}
			CHECK(nc >= 7 && ng >= 1 && ng <= (nc + 2) / 3,
			      "a GPU that stays awake 10 s was read %d times in %d samples", ng, nc);
		}
		put_sys("class/drm/card0/device/gpu_busy_percent", NULL);
		put_sys("class/drm/card0/device/power/runtime_status", NULL);
		CHECK(wait_value("cpu=20 temp=47", VALUE_MS + 3 * SAMPLE_MS), "the usage of a GPU that went away was still published");
		CHECK(wait_look(l_temp47, 3000), "dwm drew a GPU that went away");
		responsive("a GPU going away");

		/*
		 * An awake Intel GPU whose PMU counters the sampler cannot open, as
		 * without CAP_PERFMON (here the fake PMU is not the kernel's): it is
		 * never published, awake or asleep.
		 */
		set_resources("dwm.showsysmon: 0\n");
		CHECK(wait_samplers(0, 3000, NULL), "the sampler kept running while the section is hidden");
		put_sys("class/drm/card1/device/power/runtime_status", "active\n");
		put_sys("bus/pci/drivers/i915/.keep", "");
		snprintf(fifo, sizeof fifo, "%s/class/drm/card1/device/driver", sys_path);
		CHECK(symlink("../../../../bus/pci/drivers/i915", fifo) == 0, "cannot link %s", fifo);
		put_sys("bus/event_source/devices/i915/type", "4242\n");
		put_sys("bus/event_source/devices/i915/cpumask", "0\n");
		put_sys("bus/event_source/devices/i915/events/rcs0-busy", "config=0x0\n");
		set_resources("dwm.showsysmon: 1\n");
		CHECK(wait_samplers(1, 3000, &sampler_pid), "showing the section did not start the sampler");
		CHECK(wait_value("cpu=20 temp=47", VALUE_MS), "the sampler did not publish beside an Intel GPU");
		for (i = 0; i < 2 * SAMPLE_MS / 100; i++) {
			if (read_value(raw, sizeof raw) && strstr(raw, "gpu"))
				break;
			pump(100);
		}
		CHECK(!strstr(raw, "gpu"), "an Intel GPU whose counters cannot be opened was published: \"%s\"", raw);
		put_sys("class/drm/card1/device/power/runtime_status", "suspended\n");
		for (i = 0; i < 2 * SAMPLE_MS / 100; i++) {
			if (read_value(raw, sizeof raw) && strstr(raw, "gpu"))
				break;
			pump(100);
		}
		CHECK(!strstr(raw, "gpu"), "a suspended Intel GPU whose counters were never opened was published: \"%s\"", raw);
		CHECK(wait_look(l_temp47, 3000), "dwm drew an Intel GPU whose counters cannot be opened");
	}

	/* a sampler started by hand takes over from the one dwm started */
	pid = sampler_pid;
	start_sampler(argv[2]);
	CHECK(wait_only(sampler_pid, 3000), "a sampler started by hand did not take over from dwm's");
	CHECK(wait_value("cpu=20 temp=47", VALUE_MS), "the sampler started by hand did not publish");

	/* terminating the sampler removes its value; dwm shows "-" and keeps working */
	kill(sampler_pid, SIGTERM);
	CHECK(waitpid(sampler_pid, &status, 0) == sampler_pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "dwmsysmon did not exit cleanly on SIGTERM");
	sampler_pid = 0;
	CHECK(wait_look(l_dash, 2000), "dwm kept showing a value after the sampler exited");
	responsive("the sampler exiting");

	/* leave the shared test dwm as found */
	set_resources("dwm.showsysmon: 0\n");
	CHECK(wait_look(l_off, 3000), "the section was not hidden at the end");
	CHECK(samplers(NULL) == 0, "a sampler is left running with the section hidden");
	if (sampler_pid > 0)
		kill(sampler_pid, SIGKILL);
	XCloseDisplay(dpy);
	if (failures)
		return EXIT_FAILURE;
	puts("system monitor integration checks passed");
	return EXIT_SUCCESS;
}
