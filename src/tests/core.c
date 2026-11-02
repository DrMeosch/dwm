/* Headless checks for state and branch logic in dwm.c. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef VERSION
#define VERSION "test"
#endif
/* stand in for "pidof -s dwmblocks": a NULL popen, or a stream with set output */
static const char *pidof_output;
static FILE *
pidof_popen(const char *command, const char *mode)
{
	FILE *fp;

	(void)command; (void)mode;
	if (!pidof_output)
		return NULL;
	if ((fp = tmpfile())) {
		fputs(pidof_output, fp);
		rewind(fp);
	}
	return fp;
}
/* runAutostart() hands its command to system(); keep it instead of running it */
static char autostart_command[256];
static int
autostart_system(const char *command)
{
	snprintf(autostart_command, sizeof autostart_command, "%s", command);
	return 0;
}
/* spawn() and the flag applysysmon() publishes, recorded instead of reaching a process or X */
static const char *spawned[8];
static int nspawned;
static unsigned long sysmonflag = 2, sysmoninterval;
#define posix_spawnp record_spawnp
#define XChangeProperty record_changeproperty
#define XSync record_sync
#define popen pidof_popen
#define pclose fclose
#define system autostart_system
#define main dwm_program_main
#include "../dwm.c"
#undef system
#undef popen
#undef pclose
#undef main
#undef XSync
#undef XChangeProperty
#undef posix_spawnp

int
record_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *actions,
	const posix_spawnattr_t *attr, char *const argv[], char *const envp[])
{
	(void)pid; (void)actions; (void)attr; (void)argv; (void)envp;
	if (nspawned < (int)LENGTH(spawned))
		spawned[nspawned] = file;
	nspawned++;
	return 0;
}

int
record_changeproperty(Display *d, Window w, Atom property, Atom type, int format, int mode,
	const unsigned char *data, int n)
{
	(void)d; (void)w; (void)type; (void)mode;
	if (property == sysmonenabledatom && format == 32 && n == 3) {
		sysmonflag = ((const unsigned long *)data)[0];
		sysmoninterval = ((const unsigned long *)data)[2];
	}
	return 1;
}

int
record_sync(Display *d, Bool discard)
{
	(void)d; (void)discard;
	return 1;
}

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
free_monitor(Monitor *m)
{
	free(m->pertag);
	free(m);
}

static void
test_createmon(void)
{
	Monitor *m = createmon();
	unsigned int i;

	CHECK(m->tagset[0] == 1 && m->tagset[1] == 1,
	      "new monitor does not start on tag 1");
	CHECK(m->mfact == mfact && m->nmaster == nmaster,
	      "new monitor did not inherit layout defaults");
	CHECK(m->lt[0] == &layouts[defaultlayout] && m->lt[1] == &layouts[1],
	      "new monitor did not select the configured layouts");
	CHECK(m->lt[0]->arrange == righttile,
	      "new monitor does not start on righttile");
	CHECK(strcmp(m->ltsymbol, layouts[defaultlayout].symbol) == 0,
	      "new monitor has the wrong layout symbol");
	for (i = 0; i <= LENGTH(tags); i++)
		CHECK(m->pertag->ltidxs[i][0] == m->lt[0]
		      && m->pertag->ltidxs[i][1] == m->lt[1],
		      "per-tag slot %u did not inherit the default layouts", i);
	CHECK(m->pertag->curtag == 1 && m->pertag->prevtag == 1,
	      "new monitor has the wrong per-tag cursor");
	CHECK(m->pertag->nmasters[0] == nmaster
	      && m->pertag->nmasters[LENGTH(tags)] == nmaster,
	      "per-tag master counts were not initialized");
	free_monitor(m);
}

static void
test_copyvalidchars(void)
{
	char out[64];

	copyvalidchars(out, "ok\tbad\n\r\001 done");
	CHECK(strcmp(out, "okbad done") == 0,
	      "copyvalidchars did not remove control characters: %s", out);
	copyvalidchars(out, "utf-8: \303\251");
	CHECK(strcmp(out, "utf-8: \303\251") == 0,
	      "copyvalidchars damaged a non-ASCII byte sequence");
}

/* statuscode() must never read past the n bytes it is given */
static size_t
statuscode_exact(const char *text, size_t n)
{
	char *copy = malloc(n ? n : 1);
	size_t len;

	if (!copy)
		return (size_t)-1;
	memcpy(copy, text, n);
	len = statuscode(copy, n);
	free(copy);
	return len;
}

static void
test_status2d(void)
{
	static const struct { const char *text; size_t len; } codes[] = {
		{ "^c#ff0000^", 10 }, { "^b#00FfaA^", 10 }, { "^C0^", 4 }, { "^B15^", 5 },
		{ "^C007^", 6 }, { "^d^", 3 }, { "^f20^", 5 }, { "^f9999^", 7 },
		{ "^r1,2,30,4^", 11 },
		/* truncated, malformed and out-of-range codes are plain text */
		{ "^c#ff00^", 0 }, { "^c#ff00zz^", 0 }, { "^c#ff0000", 0 }, { "^c#ff0000x", 0 },
		{ "^c#ff00000^", 0 }, { "^c#zzzzzz^", 0 }, { "^c ff0000^", 0 },
		{ "^C16^", 0 }, { "^B99^", 0 }, { "^C^", 0 }, { "^C-1^", 0 }, { "^C1", 0 },
		{ "^f^", 0 }, { "^f-3^", 0 }, { "^f12345^", 0 }, { "^f1", 0 },
		{ "^r1,2,3^", 0 }, { "^r1,2,3,^", 0 }, { "^r1,2,3,4", 0 }, { "^r,,,^", 0 },
		{ "^r1;2;3;4^", 0 }, { "^r1,2,3,12345^", 0 },
		{ "^x^", 0 }, { "^D^", 0 }, { "^^", 0 }, { "^", 0 }, { "^d", 0 }, { "^dx", 0 },
		{ "c#ff0000^", 0 }, { "", 0 },
	};
	static const char palette_text[] = "^C2^^B3^";
	size_t i, n;
	char text[256];

	for (i = 0; i < LENGTH(codes); i++) {
		n = strlen(codes[i].text);
		CHECK(statuscode_exact(codes[i].text, n) == codes[i].len,
		      "statuscode(\"%s\") = %zu, want %zu", codes[i].text,
		      statuscode_exact(codes[i].text, n), codes[i].len);
		/* a code is recognized only with all of its bytes available */
		if (codes[i].len)
			for (n = 0; n < codes[i].len; n++)
				CHECK(statuscode_exact(codes[i].text, n) == 0,
				      "statuscode(\"%s\") accepted a code cut to %zu bytes",
				      codes[i].text, n);
	}
	CHECK(statuscode(palette_text, strlen(palette_text)) == 4
	      && statuscode(palette_text + 4, strlen(palette_text) - 4) == 4,
	      "statuscode did not stop at the end of a code");

	CHECK(validcolor("#09afAF") && validcolor("#000000^"),
	      "validcolor rejected a #rrggbb color");
	CHECK(!validcolor("") && !validcolor("#") && !validcolor("#12345")
	      && !validcolor("#12345g") && !validcolor("123456#") && !validcolor("ff0000"),
	      "validcolor accepted a malformed color");

	/* Without fonts, plain text has no width, so only ^f contributes. */
	{
		static Drw stub;

		drw = &stub;
		lrpad = 0;
		strcpy(text, "^c#112233^^f10^^r0,0,5,5^^d^^f7^^c#zzzzzz^");
		CHECK(drawstatus(text, strlen(text), 0, 0) == 17,
		      "drawstatus measured %d, want 17", drawstatus(text, strlen(text), 0, 0));
		CHECK(drawstatus(text, 15, 0, 0) == 10 && strlen(text) > 15 && text[15] == '^',
		      "drawstatus did not stop at the given length or restored the text");
		CHECK(drawstatus(text, 14, 0, 0) == 0,
		      "drawstatus honored a code cut by the length");
		CHECK(drawstatus(text, 0, 0, 0) == 0, "drawstatus measured an empty range");
		strcpy(text, "^f10^^f10^^f");
		CHECK(drawstatus(text, strlen(text), 5, 0) == 20,
		      "drawstatus did not measure relative to the start");

		/* clicks map through the same walker; codes themselves take no space */
		strcpy(rawstext, "\001^f50^\002^f30^\003^f20^");
		CHECK(statusblocksig(100, 99) == 0 && statusblocksig(100, 100) == 1
		      && statusblocksig(100, 149) == 1 && statusblocksig(100, 150) == 2
		      && statusblocksig(100, 179) == 2 && statusblocksig(100, 180) == 3
		      && statusblocksig(100, 500) == 3,
		      "statusblocksig mapped a click to the wrong block");
		strcpy(rawstext, "no signal ^f50^");
		CHECK(statusblocksig(0, 10) == 0, "statusblocksig invented a block");
		rawstext[0] = '\0';
		drw = NULL;
	}

	/* palette: only exactly #rrggbb replaces the default */
	{
		XrmDatabase db;

		XrmInitialize();
		db = XrmGetStringDatabase("dwm.color1: #123456\n"
		                          "dwm.color2: #12345\n"
		                          "dwm.color3: #1234567\n"
		                          "dwm.color4: 123456\n"
		                          "dwm.color5: #12345g\n"
		                          "dwm.color9: #ABCDEF\n"
		                          "dwm.color15: #000000\n"
		                          "dwm.color16: #ffffff\n");
		loadstatuspalette(NULL);
		CHECK(!strcmp(statuspalette[1], statuspalette_default[1]),
		      "loadstatuspalette did not set the defaults");
		loadstatuspalette(db);
		CHECK(!strcmp(statuspalette[1], "#123456") && !strcmp(statuspalette[9], "#ABCDEF")
		      && !strcmp(statuspalette[15], "#000000"),
		      "loadstatuspalette ignored a valid dwm.color<n>: %s %s %s",
		      statuspalette[1], statuspalette[9], statuspalette[15]);
		for (i = 2; i <= 5; i++)
			CHECK(!strcmp(statuspalette[i], statuspalette_default[i]),
			      "loadstatuspalette accepted a bad dwm.color%zu: %s", i, statuspalette[i]);
		CHECK(!strcmp(statuspalette[0], statuspalette_default[0]),
		      "loadstatuspalette changed an unset color");
		XrmDestroyDatabase(db);
		/* a reload without the resources restores the defaults */
		db = XrmGetStringDatabase("dwm.color1: bogus\n");
		loadstatuspalette(db);
		CHECK(!strcmp(statuspalette[1], statuspalette_default[1])
		      && !strcmp(statuspalette[9], statuspalette_default[9]),
		      "loadstatuspalette kept a stale color across a reload");
		XrmDestroyDatabase(db);
	}
}

static void
test_client_lists(void)
{
	Monitor m = {0};
	Client first = {.tags = 1, .win = 11, .mon = &m};
	Client hidden = {.tags = 2, .win = 12, .mon = &m};
	Client floating = {.tags = 1, .isfloating = 1, .win = 13, .mon = &m};
	Client last = {.tags = 1, .win = 14, .mon = &m};

	m.tagset[0] = m.tagset[1] = 1;
	m.clients = &first;
	first.next = &hidden;
	hidden.next = &floating;
	floating.next = &last;
	m.stack = &first;
	first.snext = &hidden;
	hidden.snext = &floating;
	floating.snext = &last;
	m.sel = &first;

	CHECK(nexttiled(m.clients) == &first, "nexttiled skipped a visible client");
	CHECK(nexttiled(first.next) == &last,
	      "nexttiled did not skip hidden and floating clients");

	mons = selmon = &m;
	CHECK(wintoclient(12) == &hidden, "wintoclient missed a client");
	m.barwin = 99;
	root = 100;
	CHECK(wintomon(99) == &m, "wintomon missed a monitor bar");
	CHECK(wintomon(13) == &m, "wintomon missed a client monitor");

	Client swallowed = {.win = 77, .mon = &m};
	first.swallowing = &swallowed;
	CHECK(swallowingclient(77) == &first,
	      "swallowingclient missed the swallowing terminal");
	first.swallowing = NULL;

	mons = selmon = NULL;
}

static void
test_attach_and_detach(void)
{
	Monitor m = {0};
	Client first = {.tags = 1, .mon = &m};
	Client second = {.tags = 1, .mon = &m};
	Client third = {.tags = 2, .mon = &m};

	m.tagset[0] = m.tagset[1] = 1;
	m.clients = &first;
	attach(&second);
	CHECK(m.clients == &second && second.next == &first,
	      "attach did not prepend a client");
	detach(&second);
	CHECK(m.clients == &first && first.next == NULL,
	      "detach did not unlink the head client");

	m.stack = &first;
	m.sel = &first;
	attachstack(&second);
	attachstack(&third);
	CHECK(m.stack == &third && third.snext == &second,
	      "attachstack did not prepend stack entries");
	m.sel = &third;
	detachstack(&third);
	CHECK(m.stack == &second && m.sel == &second,
	      "detachstack did not select the next visible client");
	detachstack(&second);
	CHECK(m.stack == &first && m.sel == &first,
	      "detachstack did not select the remaining visible client");
	detachstack(&first);
	CHECK(m.stack == NULL && m.sel == NULL,
	      "detachstack did not clear an empty selection");
}

static void
test_geometry_helpers(void)
{
	Monitor left = {.wx = 0, .wy = 0, .ww = 500, .wh = 400};
	Monitor right = {.wx = 500, .wy = 0, .ww = 500, .wh = 400};
	Client c = {.mon = &left, .bw = 2, .w = 50, .h = 40};
	Layout layout = {"test", tile};
	int x, y, w, h;

	left.next = &right;
	mons = selmon = &left;
	CHECK(recttomon(50, 20, 100, 100) == &left,
	      "recttomon chose the wrong monitor for the left rectangle");
	CHECK(recttomon(450, 20, 200, 100) == &right,
	      "recttomon did not choose the monitor with the most area");
	CHECK(dirtomon(1) == &right && dirtomon(-1) == &right,
	      "dirtomon failed at the first monitor boundary");
	selmon = &right;
	CHECK(dirtomon(1) == &left && dirtomon(-1) == &left,
	      "dirtomon did not wrap around the monitor list");

	left.lt[0] = &layout;
	left.sellt = 0;
	left.wx = 10;
	left.wy = 20;
	left.ww = 400;
	left.wh = 300;
	c.mon = &left;
	c.x = 30;
	c.y = 40;
	c.w = 50;
	c.h = 40;
	c.isfloating = 1;
	c.hintsvalid = 1;
	bh = 20;
	sw = 800;
	sh = 600;
	x = 500;
	y = 500;
	w = -4;
	h = -3;
	CHECK(applysizehints(&c, &x, &y, &w, &h, 0),
	      "applysizehints did not report a changed rectangle");
	CHECK(x == 356 && y == 276 && w == 20 && h == 20,
	      "applysizehints boundary clamp was (%d,%d %dx%d)", x, y, w, h);

	c.basew = 10;
	c.baseh = 10;
	c.incw = 10;
	c.inch = 10;
	c.minw = 20;
	c.minh = 20;
	c.maxw = 140;
	c.maxh = 100;
	x = 30;
	y = 40;
	w = 137;
	h = 97;
	CHECK(applysizehints(&c, &x, &y, &w, &h, 0),
	      "applysizehints did not apply size increments");
	CHECK(w == 130 && h == 90,
	      "applysizehints increment result was %dx%d", w, h);

	c.basew = c.baseh = c.incw = c.inch = 0;
	c.maxw = c.maxh = 0;
	c.mina = 0.5f;
	c.maxa = 1.0f;
	x = 30;
	y = 40;
	w = 200;
	h = 100;
	applysizehints(&c, &x, &y, &w, &h, 0);
	CHECK(w == 100 && h == 100,
	      "applysizehints aspect clamp was %dx%d", w, h);

	mons = selmon = NULL;
}

static void
test_bar_geometry(void)
{
	Monitor m = {.my = 10, .mh = 200, .showbar = 1, .topbar = 1};

	bh = 20;
	updatebarpos(&m);
	CHECK(m.wy == 30 && m.wh == 180 && m.by == 10,
	      "top bar geometry was y=%d h=%d by=%d", m.wy, m.wh, m.by);
	m.topbar = 0;
	updatebarpos(&m);
	CHECK(m.wy == 10 && m.wh == 180 && m.by == 190,
	      "bottom bar geometry was y=%d h=%d by=%d", m.wy, m.wh, m.by);
	m.showbar = 0;
	updatebarpos(&m);
	CHECK(m.wy == 10 && m.wh == 200 && m.by == -20,
	      "hidden bar geometry was y=%d h=%d by=%d", m.wy, m.wh, m.by);
}

static void
test_stackpos(void)
{
	Monitor m = {0};
	Client first = {.tags = 1, .mon = &m};
	Client hidden = {.tags = 2, .mon = &m};
	Client last = {.tags = 1, .mon = &m};
	Arg arg;

	m.tagset[0] = m.tagset[1] = 1;
	m.clients = &first;
	first.next = &hidden;
	hidden.next = &last;
	m.stack = &last;
	last.snext = &first;
	m.sel = &first;
	mons = selmon = &m;

	arg.i = PREVSEL;
	CHECK(stackpos(&arg) == 1, "stackpos PREVSEL returned %d", stackpos(&arg));
	arg.i = INC(1);
	CHECK(stackpos(&arg) == 1, "stackpos increment returned %d", stackpos(&arg));
	arg.i = -1;
	CHECK(stackpos(&arg) == 1, "stackpos negative returned %d", stackpos(&arg));
	arg.i = 0;
	CHECK(stackpos(&arg) == 0, "stackpos absolute returned %d", stackpos(&arg));
	mons = selmon = NULL;
}

static void
test_xresources(void)
{
	char name[] = "hello";
	int integer = 3;
	float real = 0.25f;
	XrmDatabase db;

	XrmInitialize();
	db = XrmGetStringDatabase("dwm.name: world\n"
	                          "dwm.integer: 17\n"
	                          "dwm.real: 0.75\n");
	resource_load(db, "name", STRING, name, sizeof name);
	resource_load(db, "integer", INTEGER, &integer, sizeof integer);
	resource_load(db, "real", FLOAT, &real, sizeof real);
	CHECK(strcmp(name, "world") == 0 && integer == 17
	      && fabsf(real - 0.75f) < 0.001f,
	      "resource_load decoded values incorrectly: %s %d %.2f", name, integer, real);
	resource_load(db, "missing", INTEGER, &integer, sizeof integer);
	CHECK(integer == 17, "resource_load changed a missing preference");
	XrmDestroyDatabase(db);

	{
		/* guard bytes after the buffer must survive an overlong value */
		struct { char color[COLORLEN]; char guard[8]; } buf = { "#282a36", "guard!!" };

		db = XrmGetStringDatabase("dwm.color: #123456789\n"
		                          "dwm.exact: #abcdef\n");
		resource_load(db, "color", STRING, buf.color, sizeof buf.color);
		CHECK(strcmp(buf.color, "#282a36") == 0 && strcmp(buf.guard, "guard!!") == 0,
		      "resource_load accepted an overlong color: %s", buf.color);
		resource_load(db, "exact", STRING, buf.color, sizeof buf.color);
		CHECK(strcmp(buf.color, "#abcdef") == 0 && strcmp(buf.guard, "guard!!") == 0,
		      "resource_load rejected a color that fits: %s", buf.color);
		XrmDestroyDatabase(db);
	}
}

static void
test_scratchpad_tag_views(void)
{
	Monitor *m = createmon();
	Arg arg;
	unsigned int onlysp = SPTAG(0);

	mons = selmon = m;

	/* Hiding the last normal tag while a scratchpad is shown must be refused. */
	m->tagset[m->seltags] = 1 | onlysp;
	arg.ui = 1;
	toggleview(&arg);
	CHECK(m->tagset[m->seltags] == (1 | onlysp) && m->pertag->curtag == 1,
	      "toggleview left only a scratchpad tag: tagset %#x curtag %u",
	      m->tagset[m->seltags], m->pertag->curtag);

	/* A view request naming only scratchpad tags has no per-tag slot. */
	m->tagset[m->seltags] = 1;
	m->tagset[m->seltags ^ 1] = 1;
	arg.ui = onlysp;
	view(&arg);
	CHECK(m->tagset[m->seltags] == 1 && m->pertag->curtag == 1
	      && m->pertag->curtag <= LENGTH(tags),
	      "view accepted a scratchpad-only tagset: tagset %#x curtag %u",
	      m->tagset[m->seltags], m->pertag->curtag);

	mons = selmon = NULL;
	free_monitor(m);
}

static void
test_process_helpers(void)
{
	pid_t parent = getparentprocess(getpid());

	CHECK(parent != 0, "getparentprocess could not read the current process");
	CHECK(isdescprocess(parent, getpid()) == parent,
	      "isdescprocess rejected an immediate parent");
	CHECK(isdescprocess(getpid(), parent) == 0,
	      "isdescprocess accepted an ancestor in the wrong direction");
	CHECK(getparentprocess((pid_t)-1) == 0,
	      "getparentprocess accepted an invalid pid");
}

static void
test_terminal_lookup(void)
{
	Monitor m = {0};
	Client terminal = {.isterminal = 1, .pid = getparentprocess(getpid()), .mon = &m};
	Client application = {.pid = getpid(), .mon = &m};

	m.clients = &terminal;
	mons = selmon = &m;
	CHECK(termforwin(&application) == &terminal,
	      "termforwin did not find the terminal ancestor");
	application.isterminal = 1;
	CHECK(termforwin(&application) == NULL,
	      "termforwin returned a terminal for a terminal client");
	mons = selmon = NULL;
}

static void
test_xerror_filters(void)
{
	XErrorEvent event = {0};

	event.error_code = BadWindow;
	CHECK(xerror(NULL, &event) == 0, "xerror did not ignore BadWindow");
	CHECK(xerrordummy(NULL, &event) == 0, "xerrordummy returned an error");
}

static void
test_selected_tag_fonts_disabled(void)
{
	const char *saved = selfontweight;

	/* drw is NULL here, so touching it would crash instead of returning NULL */
	drw = NULL;
	selfontweight = NULL;
	CHECK(createselfonts() == NULL, "createselfonts opened fonts for a NULL weight");
	selfontweight = "";
	CHECK(createselfonts() == NULL, "createselfonts opened fonts for an empty weight");
	selfontweight = saved;
}

/* leaves a plausible PID in the stack memory the next call is likely to reuse */
static void
poison_stack(void)
{
	volatile char junk[256];
	size_t i;

	for (i = 0; i + 6 <= sizeof junk - 1; i += 6)
		memcpy((char *)junk + i, "31337\n", 6);
	junk[sizeof junk - 1] = '\0';
}

static void
test_getdwmblockspid(void)
{
	/* dwmblocks not running: pidof prints nothing, so no PID may be parsed */
	pidof_output = "";
	dwmblockspid = 1;
	poison_stack();
	CHECK(getdwmblockspid() == -1 && dwmblockspid == 0,
	      "an empty pidof result gave PID %d", (int)dwmblockspid);

	/* popen itself failing */
	pidof_output = NULL;
	dwmblockspid = 1;
	poison_stack();
	CHECK(getdwmblockspid() == -1 && dwmblockspid == 0,
	      "a failed popen gave PID %d", (int)dwmblockspid);

	/* dwmblocks running */
	pidof_output = "4242\n";
	CHECK(getdwmblockspid() == 0 && dwmblockspid == 4242,
	      "a running dwmblocks gave PID %d", (int)dwmblockspid);
}

/* click a 1200px bar showing the given status; block 0 in clicks means no signal */
static void
check_status_clicks(const char *status, const int (*clicks)[2], size_t n, const char *what)
{
	Monitor m = {0};
	XEvent ev = {0};
	sigset_t set;
	siginfo_t info;
	struct timespec none = { 0, 0 };
	char pid[32];
	size_t i;
	int got;

	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	sigprocmask(SIG_BLOCK, &set, NULL);
	snprintf(pid, sizeof pid, "%d\n", (int)getpid());
	pidof_output = pid; /* "dwmblocks" is this process */
	dwmblockspid = 0;
	m.ww = 1200;
	m.barwin = 1;
	mons = selmon = &m;
	/* drw is NULL, so TEXTW is 0: no tags are shown and the status is placed by its width alone */
	strcpy(rawstext, status);
	copyvalidchars(stext, rawstext);
	for (i = 0; i < n; i++) {
		ev.xbutton.window = m.barwin;
		ev.xbutton.button = Button1;
		ev.xbutton.x = clicks[i][0];
		buttonpress(&ev);
		got = sigtimedwait(&set, &info, &none) == SIGUSR1 ? info.si_value.sival_int : 0;
		CHECK(got == (clicks[i][1] ? clicks[i][1] << 8 | 1 : 0),
		      "a click at x=%d on a %s was sent as 0x%x, expected block %d",
		      clicks[i][0], what, got, clicks[i][1]);
	}
	mons = selmon = NULL;
	dwmblockspid = 0;
}

/* a click reaches dwmblocks with the block under the pointer; block 0 means no signal */
static void
test_buttonpress_status(void)
{
	/* wider than the bar: the status starts at -202 and ends at 1197 */
	static const int wide[][2] = { { 700, 1 }, { 797, 1 }, { 798, 2 }, { 1180, 2 } };
	/* 200px of status and 2px of padding: block 1 is [998,1097], block 2 is [1098,1197] */
	static const int narrow[][2] = { { 997, 0 }, { 998, 1 }, { 1097, 1 }, { 1098, 2 }, { 1197, 2 } };

	check_status_clicks("\001^f1000^\002^f400^", wide, LENGTH(wide),
	                    "status wider than the bar");
	check_status_clicks("\001^f100^\002^f100^", narrow, LENGTH(narrow),
	                    "status narrower than the bar");
}

/* a click on the section or the gap up to the status is no title, tag, layout or status click */
static void
test_buttonpress_sysmon(void)
{
	/* 100px text padding: the layout symbol ends at 100, the section is [550,650), the status [1098,1200) */
	static const struct { int x, click; } at[] = {
		{ 99, ClkLtSymbol }, { 100, ClkWinTitle }, { 549, ClkWinTitle },
		{ 550, ClkLast }, { 649, ClkLast }, { 650, ClkLast }, { 1097, ClkLast },
		{ 1098, ClkStatusText }, { 1197, ClkStatusText },
	};
	static const int blocks[][2] = { { 549, 0 }, { 550, 0 }, { 1097, 0 }, { 1098, 1 }, { 1197, 1 } };
	Monitor m = {0};
	Arg arg = {0};
	int saved_lrpad = lrpad, saved_bh = bh, saved_show = showsysmon, saved_graph = sysmongraph;
	size_t i;
	int got;

	lrpad = 100;
	bh = 20;
	sysmongraph = 0;
	m.ww = 1200;
	m.barwin = 1;
	mons = selmon = &m;
	strcpy(rawstext, "\001^f100^");
	copyvalidchars(stext, rawstext);

	showsysmon = 1;
	for (i = 0; i < LENGTH(at); i++) {
		got = barclick(&m, at[i].x, &arg);
		CHECK(got == at[i].click, "a click at x=%d with the section shown was %d, expected %d",
		      at[i].x, got, at[i].click);
	}
	check_status_clicks("\001^f100^", blocks, LENGTH(blocks), "status beside the section");
	lrpad = 100;
	bh = 20;
	mons = selmon = &m;

	showsysmon = 0;
	CHECK(barclick(&m, 600, &arg) == ClkWinTitle, "a click at x=600 with the section off was not a title click");

	/* a status reaching into the centre leaves the section out, so its pixels belong to the title */
	showsysmon = 1;
	strcpy(rawstext, "\001^f600^");
	copyvalidchars(stext, rawstext);
	CHECK(barclick(&m, 500, &arg) == ClkWinTitle, "a click at x=500 with the section left out was not a title click");

	/* the graph is part of the section: 16 bars of 2px make it [534,666) */
	sysmongraph = 32;
	strcpy(rawstext, "\001^f100^");
	copyvalidchars(stext, rawstext);
	CHECK(barclick(&m, 533, &arg) == ClkWinTitle, "a click left of the section with its graph was not a title click");
	CHECK(barclick(&m, 534, &arg) == ClkLast && barclick(&m, 640, &arg) == ClkLast
	      && barclick(&m, 665, &arg) == ClkLast, "a click on the section with its graph was a title click");

	showsysmon = saved_show;
	sysmongraph = saved_graph;
	lrpad = saved_lrpad;
	bh = saved_bh;
	mons = selmon = NULL;
}

/* the section is centred and never overlaps the layout symbol on its left or the status on its right */
static void
test_sysmon_box(void)
{
	Monitor m = {0};
	Arg arg = {0};
	int saved_lrpad = lrpad, saved_bh = bh, saved_show = showsysmon, saved_graph = sysmongraph;
	int mx = -1, bars = -1;

	lrpad = 100; /* drw is NULL, so the section is 100px wide without its graph */
	bh = 20;
	sysmongraph = 0;
	showsysmon = 1;
	m.barwin = 1;
	mons = selmon = &m;
	strcpy(rawstext, "");
	copyvalidchars(stext, rawstext);

	m.ww = 1200;
	CHECK(sysmonbox(&m, 100, 1198, &mx, &bars) == 100 && mx == 550 && bars == 0, "a wide bar did not centre the section (x=%d)", mx);
	/* the layout symbol ends at 100: the title keeps more than a bar height only from a 342px bar */
	m.ww = 340;
	CHECK(sysmonbox(&m, 100, 338, &mx, &bars) == 0, "the section left only a bar height of title");
	m.ww = 342;
	CHECK(sysmonbox(&m, 100, 340, &mx, &bars) == 100 && mx == 121, "the section was left out although the title keeps more than a bar height");
	/* a narrow bar: the centred section would sit on the layout symbol, with room to spare on the right */
	m.ww = 240;
	CHECK(sysmonbox(&m, 100, 238, &mx, &bars) == 0, "the section was drawn over the layout symbol of a narrow bar");
	CHECK(barclick(&m, 100, &arg) == ClkWinTitle && barclick(&m, 150, &arg) == ClkWinTitle,
	      "clicks right of the layout symbol of a narrow bar were not title clicks");
	m.ww = 130;
	CHECK(sysmonbox(&m, 100, 128, &mx, &bars) == 0, "the section was drawn on a bar barely wider than the layout symbol");
	/* a status reaching into the centre */
	m.ww = 1200;
	CHECK(sysmonbox(&m, 100, 600, &mx, &bars) == 0, "the section was drawn over the status");
	CHECK(sysmonbox(&m, 100, 650, &mx, &bars) == 100, "the section was left out although it ends where the status begins");
	CHECK(sysmonbox(&m, 100, 649, &mx, &bars) == 0, "the section was drawn over the first pixel of the status");
	showsysmon = 0;
	CHECK(sysmonbox(&m, 100, 1198, &mx, &bars) == 0, "a hidden section took room");

	/*
	 * The graph: up to 16 bars, sysmongraph / 16 px each, beside the CPU
	 * usage. Short of room it gives up its oldest bars, down to none, before
	 * the section is left out.
	 */
	showsysmon = 1;
	sysmongraph = 32;
	CHECK(sysmonbars() == 16 && SYSMONBARW == 2, "a 32px graph has %d bars of %dpx", sysmonbars(), SYSMONBARW);
	CHECK(sysmonbox(&m, 100, 1198, &mx, &bars) == 132 && mx == 534 && bars == 16,
	      "a wide bar did not centre the section with its graph (x=%d, %d bars)", mx, bars);
	m.ww = 360;
	CHECK(sysmonbox(&m, 100, 358, &mx, &bars) == 118 && mx == 121 && bars == 9,
	      "a narrow bar did not shrink the graph to what fits (x=%d, %d bars)", mx, bars);
	m.ww = 342;
	CHECK(sysmonbox(&m, 100, 340, &mx, &bars) == 100 && mx == 121 && bars == 0,
	      "the graph was not left out before the section (x=%d, %d bars)", mx, bars);
	m.ww = 340;
	CHECK(sysmonbox(&m, 100, 338, &mx, &bars) == 0, "the section with its graph left only a bar height of title");
	m.ww = 1200;
	CHECK(sysmonbox(&m, 100, 666, &mx, &bars) == 132 && bars == 16, "the graph was cut although it ends where the status begins");
	CHECK(sysmonbox(&m, 100, 665, &mx, &bars) == 130 && mx == 535 && bars == 15,
	      "the graph reached the first pixel of the status (x=%d, %d bars)", mx, bars);
	CHECK(sysmonbox(&m, 100, 650, &mx, &bars) == 100 && bars == 0, "a status near the centre did not leave the graph out");
	CHECK(sysmonbox(&m, 100, 649, &mx, &bars) == 0, "the section was drawn over the status for want of a graph");
	sysmongraph = 8;
	CHECK(sysmonbars() == 8 && SYSMONBARW == 1, "an 8px graph has %d bars of %dpx", sysmonbars(), SYSMONBARW);
	sysmongraph = 64;
	CHECK(sysmonbars() == 16 && SYSMONBARW == 4, "a 64px graph has %d bars of %dpx", sysmonbars(), SYSMONBARW);
	sysmongraph = 40;
	CHECK(sysmonbars() == 16 && SYSMONBARW == 2, "a 40px graph has %d bars of %dpx", sysmonbars(), SYSMONBARW);
	sysmongraph = -5;
	CHECK(sysmonbars() == 0 && sysmonbox(&m, 100, 1198, &mx, &bars) == 100 && bars == 0, "a negative graph width took room");
	sysmongraph = 0;
	CHECK(sysmonbars() == 0, "a graph of no width has bars");

	showsysmon = saved_show;
	sysmongraph = saved_graph;
	lrpad = saved_lrpad;
	bh = saved_bh;
	mons = selmon = NULL;
}

/*
 * The commands dwm runs at startup leave the sampler alone: dwm starts it only
 * while the section is shown, and stopping one by name would also stop the
 * samplers of other X sessions. They run here against stub programs only, with
 * nothing else on PATH.
 */
static void
test_autostart_command(void)
{
	char dir[] = "/tmp/dwm-autostart-XXXXXX", path[512], log[512], line[256];
	const char *saved_path = getenv("PATH");
	const char *tools[] = { "killall", "dwmblocks", "dwmsysmon" };
	int sampler = 0, blocks = 0, i;
	FILE *fp;

	if (!mkdtemp(dir)) {
		CHECK(0, "cannot create a scratch directory");
		return;
	}
	snprintf(log, sizeof log, "%s/log", dir);
	for (i = 0; i < 3; i++) {
		snprintf(path, sizeof path, "%s/%s", dir, tools[i]);
		if ((fp = fopen(path, "w"))) {
			fprintf(fp, "#!/bin/sh\necho \"%s $*\" >>\"%s\"\n", tools[i], log);
			fclose(fp);
			chmod(path, 0755);
		}
	}
	autostart_command[0] = '\0';
	runAutostart();
	CHECK(autostart_command[0], "dwm ran no startup command");
	setenv("PATH", dir, 1);
	if (autostart_command[0] && system(autostart_command) != 0)
		CHECK(0, "the startup command failed");
	/* the programs are started in the background; keep their stubs until they ran */
	for (i = 0; i < 100 && !blocks; i++) {
		if ((fp = fopen(log, "r"))) {
			while (fgets(line, sizeof line, fp))
				if (!strncmp(line, "dwmblocks", 9))
					blocks = 1;
			fclose(fp);
		}
		if (!blocks)
			usleep(20000);
	}
	usleep(100000); /* time for a sampler started in the background as well */
	setenv("PATH", saved_path ? saved_path : "/usr/bin:/bin", 1);
	CHECK(blocks, "the startup command did not start dwmblocks");
	if ((fp = fopen(log, "r"))) {
		while (fgets(line, sizeof line, fp))
			if (strstr(line, "dwmsysmon"))
				sampler = 1;
		fclose(fp);
	}
	CHECK(!sampler, "the startup command starts or stops samplers, whether the section is shown or not");
	for (i = 0; i < 3; i++) {
		snprintf(path, sizeof path, "%s/%s", dir, tools[i]);
		unlink(path);
	}
	unlink(log);
	rmdir(dir);
}

/*
 * The sampler runs only while the section is shown: dwm starts it once each
 * time the section turns on, at start-up, by a key or by a reload, and turning
 * the section off only clears the flag the sampler exits on.
 */
static void
test_sysmon_start(void)
{
	int saved_show = showsysmon, saved_published = sysmonpublished;

	nspawned = 0;
	sysmonflag = 2;
	sysmonpublished = -1; /* as at start-up */
	showsysmon = 0;
	applysysmon();
	CHECK(sysmonflag == 0 && nspawned == 0, "dwm started a sampler with the section off by default (%d)", nspawned);
	/* the sampler samples as often as config.h says, and dwm waits for more than two samples */
	CHECK(sysmoninterval == 4000 && sysmoninterval == SYSMONINTERVAL,
	      "dwm did not tell the sampler to sample every 4000 ms (%lu)", sysmoninterval);
	CHECK(sysmonstale > 2 * SYSMONINTERVAL, "the default staleness limit (%d ms) does not cover two samples", sysmonstale);
	applysysmon();
	CHECK(nspawned == 0, "a reload with the section off started a sampler");
	showsysmon = 1;
	applysysmon();
	CHECK(sysmonflag == 1 && nspawned == 1 && !strcmp(spawned[0], "dwmsysmon"),
	      "showing the section did not start dwmsysmon once (%d)", nspawned);
	applysysmon();
	CHECK(nspawned == 1, "a reload with the section still shown started another sampler (%d)", nspawned);
	togglesysmon(NULL);
	CHECK(sysmonflag == 0 && showsysmon == 0 && nspawned == 1,
	      "hiding the section did not tell the sampler to stop, or started one (%d)", nspawned);
	togglesysmon(NULL);
	CHECK(sysmonflag == 1 && nspawned == 2 && !strcmp(spawned[1], "dwmsysmon"),
	      "showing the section again did not start dwmsysmon (%d)", nspawned);
	nspawned = 0;
	sysmonpublished = -1;
	showsysmon = 5; /* any non-zero resource value */
	applysysmon();
	CHECK(sysmonflag == 1 && nspawned == 1, "dwm did not start a sampler with the section shown at start-up");
	showsysmon = saved_show;
	sysmonpublished = saved_published;
	sysmon_clear(&sysmonval);
	sysmonexpire = 0;
}

/* The section keeps a value only while the sampler refreshes it, and dwm sleeps without one. */
static void
test_sysmon_expiry(void)
{
	int t;

	sysmon_clear(&sysmonval);
	sysmonexpire = 0;
	CHECK(sysmontimeout() == -1, "dwm sets a poll timeout without a value to expire");
	sysmonval.cpu = 40;
	sysmonval.temp = 55;
	sysmonval.nfan = 2;
	sysmonval.fan[0] = 2400;
	sysmonval.fan[1] = 2350;
	sysmonval.nhist = 2;
	sysmonval.hist[0] = 38;
	sysmonval.hist[1] = 40;
	sysmonexpire = nowms() + 5000;
	t = sysmontimeout();
	CHECK(t > 4000 && t <= 5000, "poll timeout %d does not match the time left", t);
	checksysmon();
	CHECK(sysmonval.cpu == 40 && sysmonval.temp == 55 && sysmonval.nfan == 2 && sysmonval.nhist == 2 && sysmonexpire,
	      "a fresh value was dropped");
	sysmonexpire = nowms() - 1;
	CHECK(sysmontimeout() == 0, "an overdue value does not wake poll at once");
	checksysmon();
	CHECK(sysmonval.cpu == -1 && sysmonval.temp == -1 && sysmonval.nfan == 0 && sysmonval.nhist == 0
	      && sysmonexpire == 0 && sysmontimeout() == -1, "a stale value was not dropped and the timer disarmed");
	checksysmon();
	CHECK(sysmonval.cpu == -1, "checksysmon revived a dropped value");
}

/* The section shows the fields the sampler has, at a width that does not follow the values. */
static void
test_sysmon_text(void)
{
	static const struct { int cpu, temp, nfan, fan[SYSMON_FANS]; const char *text, *widest; } cases[] = {
		{ -1, -1, 0, { 0 }, "CPU   - ", "CPU 100%" },
		{ 7, -1, 0, { 0 }, "CPU   7%", "CPU 100%" },
		{ 100, -1, 0, { 0 }, "CPU 100%", "CPU 100%" },
		{ 42, 55, 0, { 0 }, "CPU  42%  55°C", "CPU 100% 100°C" },
		{ -1, 55, 0, { 0 }, "CPU   -   55°C", "CPU 100% 100°C" },
		{ 42, 150, 0, { 0 }, "CPU  42% 150°C", "CPU 100% 100°C" },
		{ 42, 55, 1, { 2400 }, "CPU  42%  55°C  2400rpm", "CPU 100% 100°C  9999rpm" },
		{ 42, 55, 2, { 2400, 0 }, "CPU  42%  55°C  2400/0rpm", "CPU 100% 100°C  9999/9999rpm" },
		{ 42, -1, 2, { 2400, 2350 }, "CPU  42%  2400/2350rpm", "CPU 100%  9999/9999rpm" },
		{ 42, 55, 2, { 12000, 800 }, "CPU  42%  55°C  12000/800rpm", "CPU 100% 100°C  99999/9999rpm" },
		{ 100, 150, 4, { 30000, 30000, 30000, 30000 }, "CPU 100% 150°C  30000/30000/30000/30000rpm",
		  "CPU 100% 100°C  99999/99999/99999/99999rpm" },
	};
	char text[128], widest[128], cpu[64], rest[64];
	size_t i;

	for (i = 0; i < LENGTH(cases); i++) {
		sysmonval.cpu = cases[i].cpu;
		sysmonval.temp = cases[i].temp;
		sysmonval.nfan = cases[i].nfan;
		memcpy(sysmonval.fan, cases[i].fan, sizeof sysmonval.fan);
		/* the graph goes between the CPU usage and the rest */
		sysmontext(cpu, rest, sizeof cpu, 0);
		CHECK(strlen(cpu) == 8, "the CPU usage \"%s\" is not as long as \"CPU 100%%\"", cpu);
		snprintf(text, sizeof text, "%s%s", cpu, rest);
		sysmontext(cpu, rest, sizeof cpu, 1);
		snprintf(widest, sizeof widest, "%s%s", cpu, rest);
		CHECK(!strcmp(text, cases[i].text), "the section reads \"%s\", expected \"%s\"", text, cases[i].text);
		CHECK(!strcmp(widest, cases[i].widest), "the section is as wide as \"%s\", expected \"%s\"",
		      widest, cases[i].widest);
		CHECK(strlen(text) <= strlen(widest), "\"%s\" is longer than its width \"%s\"", text, widest);
	}
	/* a small buffer is never overrun */
	sysmonval.temp = 100;
	sysmonval.nfan = 1;
	memset(cpu, 'z', sizeof cpu);
	memset(rest, 'z', sizeof rest);
	sysmontext(cpu, rest, 6, 1);
	CHECK(cpu[5] == '\0' && cpu[6] == 'z' && rest[5] == '\0' && rest[6] == 'z', "the section text overran its buffer");
	sysmon_clear(&sysmonval);
}

int
main(void)
{
	test_sysmon_expiry();
	test_sysmon_text();
	test_sysmon_box();
	test_autostart_command();
	test_sysmon_start();
	test_createmon();
	test_copyvalidchars();
	test_status2d();
	test_client_lists();
	test_attach_and_detach();
	test_geometry_helpers();
	test_bar_geometry();
	test_stackpos();
	test_xresources();
	test_scratchpad_tag_views();
	test_process_helpers();
	test_terminal_lookup();
	test_xerror_filters();
	test_selected_tag_fonts_disabled();
	test_getdwmblockspid();
	test_buttonpress_status();
	test_buttonpress_sysmon();
	if (failures)
		return EXIT_FAILURE;
	puts("core state regression checks passed");
	return EXIT_SUCCESS;
}
