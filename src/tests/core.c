/* Headless checks for state and branch logic in dwm.c. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define popen pidof_popen
#define pclose fclose
#define main dwm_program_main
#include "../dwm.c"
#undef popen
#undef pclose
#undef main

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

int
main(void)
{
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
	test_getdwmblockspid();
	test_buttonpress_status();
	if (failures)
		return EXIT_FAILURE;
	puts("core state regression checks passed");
	return EXIT_SUCCESS;
}
