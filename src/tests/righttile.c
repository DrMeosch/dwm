#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define WIDTH(c) ((c)->w + 2 * (c)->bw)
#define HEIGHT(c) ((c)->h + 2 * (c)->bw)
#define False 0

typedef struct Client Client;
typedef struct Monitor Monitor;
typedef union Arg Arg;

union Arg {
	int i;
	unsigned int ui;
	float f;
	const void *v;
};

struct Client {
	int x, y, w, h, bw;
	int isfloating;
	Client *next;
};

struct Monitor {
	char ltsymbol[16];
	int wx, wy, ww, wh;
	int gappih, gappiv, gappoh, gappov;
	int nmaster;
	float mfact;
	Client *clients;
};

static int smartgaps;
static int gappih, gappiv, gappoh, gappov;
static float mfact = 0.55f;
static Monitor *selmon;

static Client *
nexttiled(Client *c)
{
	for (; c && c->isfloating; c = c->next)
		;
	return c;
}

static void
resize(Client *c, int x, int y, int w, int h, int interact)
{
	(void)interact;
	c->x = x;
	c->y = y;
	c->w = w;
	c->h = h;
}

static void arrange(Monitor *m);
#include "../vanitygaps.c"

static void
arrange(Monitor *m)
{
	(void)m;
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
check_geometry(const char *name, Client *c, int x, int y, int w, int h)
{
	CHECK(c->x == x && c->y == y && c->w == w && c->h == h,
	      "%s geometry was (%d,%d %dx%d), expected (%d,%d %dx%d)",
	      name, c->x, c->y, c->w, c->h, x, y, w, h);
}

static void
test_righttile(void)
{
	Monitor m = {
		.wx = 100, .wy = 50, .ww = 1200, .wh = 800,
		.gappih = 20, .gappiv = 10, .gappoh = 10, .gappov = 30,
		.nmaster = 1, .mfact = 0.55f,
	};
	Client stack = {.bw = 2};
	Client master = {.bw = 2};

	m.clients = &master;
	righttile(&m);
	/* One client uses all of the work area; the two-thirds split begins with a stack. */
	check_geometry("single right master", &master, 130, 60, 1136, 776);

	master.next = &stack;
	stack.x = stack.y = stack.w = stack.h = 0;
	righttile(&m);
	check_geometry("right stack", &stack, 130, 60, 372, 776);
	check_geometry("right master", &master, 516, 60, 749, 776);
	CHECK(stack.x == m.wx + m.gappov, "left outer gap was not preserved");
	CHECK(abs(master.x + master.w + 2 * master.bw -
		  (m.wx + m.ww - m.gappov)) <= 1,
	      "right outer gap was not preserved");
	CHECK(master.x - (stack.x + stack.w + 2 * stack.bw) == m.gappiv,
	      "inner horizontal gap was not preserved");
	CHECK(abs(3 * (master.w + 2 * master.bw) -
		  2 * (m.ww - 2 * m.gappov - m.gappiv)) <= 4,
	      "right master is not approximately two-thirds of the split width");
}

static void
test_tile(void)
{
	Monitor m = {
		.wx = 100, .wy = 50, .ww = 1200, .wh = 800,
		.gappih = 20, .gappiv = 10, .gappoh = 10, .gappov = 30,
		.nmaster = 1, .mfact = 0.55f,
	};
	Client stack = {.bw = 2};
	Client master = {.bw = 2, .next = &stack};

	m.clients = &master;
	tile(&m);
	check_geometry("conventional master", &master, 130, 60, 617, 776);
	check_geometry("conventional stack", &stack, 761, 60, 504, 776);
	CHECK(master.x < stack.x, "ordinary tile no longer places master on the left");
}

static void
test_mfact_direction(void)
{
	CHECK(fabsf(adjustmfact(0.55f, -0.05f, righttile) - 0.60f) < 0.0001f,
	      "Mod+Shift+h should grow the right-side master");
	CHECK(fabsf(adjustmfact(0.55f, 0.05f, righttile) - 0.50f) < 0.0001f,
	      "Mod+Shift+l should shrink the right-side master");
	CHECK(fabsf(adjustmfact(0.55f, -0.05f, tile) - 0.50f) < 0.0001f,
	      "Mod+Shift+h should retain conventional tile behavior");
	CHECK(fabsf(adjustmfact(0.55f, 0.05f, tile) - 0.60f) < 0.0001f,
	      "Mod+Shift+l should retain conventional tile behavior");
}

int
main(void)
{
	test_righttile();
	test_tile();
	test_mfact_direction();
	if (failures)
		return EXIT_FAILURE;
	puts("right-side-main regression checks passed");
	return EXIT_SUCCESS;
}
