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
#define main dwm_program_main
#include "../dwm.c"
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

	CHECK(m->tagset[0] == 1 && m->tagset[1] == 1,
	      "new monitor does not start on tag 1");
	CHECK(m->mfact == mfact && m->nmaster == nmaster,
	      "new monitor did not inherit layout defaults");
	CHECK(m->lt[0] == &layouts[0] && m->lt[1] == &layouts[1],
	      "new monitor did not select the configured layouts");
	CHECK(strcmp(m->ltsymbol, layouts[0].symbol) == 0,
	      "new monitor has the wrong layout symbol");
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
	resource_load(db, "name", STRING, name);
	resource_load(db, "integer", INTEGER, &integer);
	resource_load(db, "real", FLOAT, &real);
	CHECK(strcmp(name, "world") == 0 && integer == 17
	      && fabsf(real - 0.75f) < 0.001f,
	      "resource_load decoded values incorrectly: %s %d %.2f", name, integer, real);
	resource_load(db, "missing", INTEGER, &integer);
	CHECK(integer == 17, "resource_load changed a missing preference");
	XrmDestroyDatabase(db);
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

int
main(void)
{
	test_createmon();
	test_copyvalidchars();
	test_client_lists();
	test_attach_and_detach();
	test_geometry_helpers();
	test_bar_geometry();
	test_stackpos();
	test_xresources();
	test_process_helpers();
	test_terminal_lookup();
	test_xerror_filters();
	if (failures)
		return EXIT_FAILURE;
	puts("core state regression checks passed");
	return EXIT_SUCCESS;
}
