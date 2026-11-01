/*
 * End-to-end checks of the system monitor section against the running test dwm:
 * a stand-in sampler (this program) and the real dwmsysmon publish through the
 * root window property, and the bar must show the values, "-" when the CPU
 * usage is missing, malformed or stale, and no temperature or fans when they
 * are, while dwm stays responsive. The section is recognised by the bar pixels
 * it produces.
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
#define SAMPLER_STALE_MS 4500 /* dwm.sysmonstale used with dwmsysmon, which samples every 2000 ms */

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
	char path[64], exe[PATH_MAX], env[16384], want[128], *p;
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

int
main(int argc, char *argv[])
{
	unsigned long h_off, h_dash, h20, h80, h42, h_two, h_one, h_nofan, h_temp47;
	char big[4096], medium[64], huge[200], stub[PATH_MAX];
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
	CHECK(reference("cpu=42 gpu=9 something=else") == h42, "unknown fields in the payload changed the value");

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
		CHECK(reference("cpu=42 temp=55 fan=2400,2350 gpu=7") == full, "an unknown field changed the sensors");
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
	 * does not fit. "cpu=100" fills the whole box, so its ink spans it.
	 */
	publish("cpu=100");
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
		};
		int lastwidth = 0;

		for (i = 0; i < (int)(sizeof payloads / sizeof *payloads); i++) {
			publish(payloads[i]);
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
	remove_value();
	CHECK(wait_hash(h_dash, 2000), "the value did not go away");

	/* the real sampler, started by dwm: values from a stat file, then hang, bad data and no data */
	set_resources("dwm.showsysmon: 0\n");
	CHECK(wait_hash(h_off, 3000), "the section did not hide before the real sampler's turn");
	provide_sampler(sampler_exe);
	set_resources("dwm.showsysmon: 1\ndwm.sysmonstale: 4500\n");
	CHECK(wait_samplers(1, 3000, &sampler_pid), "showing the section did not start dwmsysmon");
	fixture_busy = 20;
	CHECK(wait_hash(h20, 6000), "dwmsysmon's 20%% was not shown by dwm");
	/* no sysfs sensors at all: only the CPU usage is published */
	CHECK(wait_value("cpu=20", 3000), "dwmsysmon did not publish exactly cpu=20 without sensors");
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

	/* hidden again: the sampler exits and takes its value along; shown again, dwm starts a new one */
	set_resources("dwm.showsysmon: 0\n");
	CHECK(wait_hash(h_off, 3000), "turning the section off through Xresources did not restore the bar");
	CHECK(wait_samplers(0, 3000, NULL), "the sampler kept running while the section is hidden");
	CHECK(wait_value(NULL, 3000), "the sampler left its value behind while hidden");
	CHECK(enabled_flag() == 0, "dwm did not tell the sampler the section is hidden again");
	set_resources("dwm.showsysmon: 1\n");
	CHECK(wait_samplers(1, 3000, &pid) && pid != sampler_pid, "showing the section again did not start a new sampler");
	sampler_pid = pid;
	CHECK(wait_hash(h20, 8000), "the value did not return when the section was shown again");

	/*
	 * A sampler that misses the stop, as one stuck in a sensor read would, does
	 * not hold up dwm, and the sampler dwm starts next retires it.
	 */
	stuck = sampler_pid;
	kill(stuck, SIGSTOP);
	set_resources("dwm.showsysmon: 0\n");
	CHECK(wait_hash(h_off, 3000), "a stuck sampler held up hiding the section");
	responsive("hiding the section of a stuck sampler");
	set_resources("dwm.showsysmon: 1\n");
	CHECK(wait_samplers(2, 3000, NULL), "showing the section did not start a sampler beside the stuck one");
	responsive("starting a sampler beside a stuck one");
	kill(stuck, SIGCONT);
	CHECK(wait_samplers(1, 3000, &pid) && pid != stuck, "the stuck sampler was not retired by the new one");
	sampler_pid = pid;
	CHECK(wait_hash(h20, 8000), "the value did not return from the sampler that replaced a stuck one");

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
				CHECK(wait_samplers(0, 3000, NULL), "Mod+Shift+b did not stop the sampler");
				CHECK(wait_value(NULL, 3000), "the sampler stopped by Mod+Shift+b left its value behind");
			} else {
				CHECK(wait_samplers(1, 3000, &sampler_pid), "Mod+Shift+b did not start the sampler again");
				CHECK(wait_hash(h20, 8000), "Mod+Shift+b did not show the section with a value");
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
	CHECK(wait_hash(h_off, 3000), "the section did not hide before the sensors appeared");
	CHECK(wait_samplers(0, 3000, NULL), "the sampler kept running while the section is hidden");
	set_resources("dwm.showsysmon: 1\n");
	CHECK(wait_samplers(1, 3000, &sampler_pid), "showing the section did not start the sampler");
	CHECK(wait_value("cpu=20 temp=48 fan=2100,2050", 8000), "dwmsysmon did not publish the temperature and both fans");
	CHECK(wait_hash(h_two, 3000), "dwm did not show the temperature and both fans");
	put_sys("devices/hwmon2/fan2_input", NULL);
	CHECK(wait_value("cpu=20 temp=48 fan=2100", 10000), "a fan that went away was still published");
	CHECK(wait_hash(h_one, 3000), "dwm did not show the one fan left");
	put_sys("devices/hwmon2/fan1_input", NULL);
	CHECK(wait_value("cpu=20 temp=48", 10000), "fans that went away were still published");
	CHECK(wait_hash(h_nofan, 3000), "dwm did not show the temperature without fans");
	put_sys("devices/hwmon1/temp1_input", "48 degrees\n");
	CHECK(wait_value("cpu=20", 10000), "a malformed temperature was published");
	CHECK(wait_hash(h20, 3000), "dwm showed a malformed temperature");
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
		CHECK(wait_hash(h_dash, 12000 + SAMPLER_STALE_MS), "the value of a sampler stuck in a sensor read did not go stale");
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
		CHECK(wait_value("cpu=20 temp=47", 6000), "the value did not return after the slow sensor read");
		CHECK(wait_hash(h_temp47, 3000), "dwm did not show the value after the slow sensor read");
		put_sys("devices/hwmon1/temp1_input", "47000\n"); /* a plain file again, before the next read */
	}

	/* a sampler started by hand takes over from the one dwm started */
	pid = sampler_pid;
	start_sampler(argv[2]);
	CHECK(wait_only(sampler_pid, 3000), "a sampler started by hand did not take over from dwm's");
	CHECK(wait_value("cpu=20 temp=47", 6000), "the sampler started by hand did not publish");

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
	CHECK(samplers(NULL) == 0, "a sampler is left running with the section hidden");
	if (sampler_pid > 0)
		kill(sampler_pid, SIGKILL);
	XCloseDisplay(dpy);
	if (failures)
		return EXIT_FAILURE;
	puts("system monitor integration checks passed");
	return EXIT_SUCCESS;
}
