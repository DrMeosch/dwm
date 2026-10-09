/* Headless checks for every vanitygaps layout and gap branch. */
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
static int gappih = 20, gappiv = 10, gappoh = 10, gappov = 30;
static float mfact = 0.55f;
static Monitor *selmon;
static int arrange_calls;

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
	arrange_calls++;
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
reset_clients(Client *clients, size_t count)
{
	size_t i;
	for (i = 0; i < count; i++) {
		clients[i].x = clients[i].y = clients[i].w = clients[i].h = -1;
		clients[i].bw = 2;
		clients[i].isfloating = 0;
		clients[i].next = i + 1 < count ? &clients[i + 1] : NULL;
	}
}

static void
check_layout(const char *name, void (*layout)(Monitor *), unsigned int count)
{
	Monitor m = {
		.wx = 100, .wy = 50, .ww = 1200, .wh = 800,
		.gappih = 20, .gappiv = 10, .gappoh = 10, .gappov = 30,
		.nmaster = 1, .mfact = 0.55f,
	};
	Client clients[6];
	Client *c;
	unsigned int i;

	CHECK(count <= sizeof clients / sizeof clients[0], "%s test is too large", name);
	reset_clients(clients, count);
	m.clients = count ? &clients[0] : NULL;
	layout(&m);
	for (i = 0, c = m.clients; c; c = c->next, i++) {
		CHECK(c->w > 0 && c->h > 0,
		      "%s produced invalid client %u geometry (%d,%d %dx%d)",
		      name, i, c->x, c->y, c->w, c->h);
		CHECK(c->x >= m.wx && c->y >= m.wy
		      && c->x + WIDTH(c) <= m.wx + m.ww
		      && c->y + HEIGHT(c) <= m.wy + m.wh,
		      "%s placed client %u outside the monitor", name, i);
	}
}

static void
test_layouts(void)
{
	check_layout("bstack", bstack, 4);
	check_layout("centeredmaster", centeredmaster, 5);
	check_layout("centeredfloatingmaster", centeredfloatingmaster, 4);
	check_layout("deck", deck, 4);
	check_layout("dwindle", dwindle, 5);
	check_layout("spiral", spiral, 5);
	check_layout("tile", tile, 4);
	check_layout("righttile", righttile, 4);
	check_layout("empty layout", tile, 0);

	CHECK(fabsf(adjustmfact(0.55f, -0.05f, righttile) - 0.60f) < 0.0001f,
	      "righttile master-factor adjustment has the wrong direction");
	CHECK(fabsf(adjustmfact(0.55f, 0.05f, tile) - 0.60f) < 0.0001f,
	      "tile master-factor adjustment has the wrong direction");
}

static void
test_facts_and_gaps(void)
{
	Monitor m = {.nmaster = 1, .gappih = 20, .gappiv = 10,
	             .gappoh = 30, .gappov = 40};
	Client first = {.next = NULL};
	Client second = {0};
	float mf, sf;
	int mr, sr, oh, ov, ih, iv;
	unsigned int n;
	Arg arg = {.i = -50};

	m.clients = &first;
	getfacts(&m, 100, 200, &mf, &sf, &mr, &sr);
	CHECK(mf == 1.0f && sf == 0.0f && mr == 0 && sr == 200,
	      "getfacts single-master result was %.1f %.1f %d %d", mf, sf, mr, sr);

	first.next = &second;
	getfacts(&m, 100, 200, &mf, &sf, &mr, &sr);
	CHECK(mf == 1.0f && sf == 1.0f && mr == 0 && sr == 0,
	      "getfacts split result was %.1f %.1f %d %d", mf, sf, mr, sr);

	selmon = &m;
	arrange_calls = 0;
	setgaps(-1, -2, 3, 4);
	CHECK(m.gappoh == 0 && m.gappov == 0 && m.gappih == 3 && m.gappiv == 4,
	      "setgaps did not clamp negative values");
	CHECK(arrange_calls == 1, "setgaps did not arrange the selected monitor");
	incrgaps(&arg);
	CHECK(m.gappoh == 0 && m.gappov == 0 && m.gappih == 0 && m.gappiv == 0,
	      "incrgaps did not use the shared increment");

	getgaps(&m, &oh, &ov, &ih, &iv, &n);
	CHECK(oh == 0 && ov == 0 && ih == 0 && iv == 0 && n == 2,
	      "getgaps returned %d %d %d %d %u", oh, ov, ih, iv, n);
	defaultgaps(NULL);
	CHECK(m.gappoh == gappoh && m.gappov == gappov
	      && m.gappih == gappih && m.gappiv == gappiv,
	      "defaultgaps did not restore configured gaps");

	arrange_calls = 0;
	togglegaps(NULL);
	getgaps(&m, &oh, &ov, &ih, &iv, &n);
	CHECK(oh == 0 && ov == 0 && ih == 0 && iv == 0,
	      "togglegaps did not disable all gaps");
	CHECK(arrange_calls == 1, "togglegaps did not arrange all monitors");
	togglegaps(NULL);
	smartgaps = 1;
	m.clients->next = NULL;
	getgaps(&m, &oh, &ov, &ih, &iv, &n);
	CHECK(oh == 0 && ov == 0 && ih == gappih && iv == gappiv,
	      "smartgaps did not remove outer gaps for one client");
	smartgaps = 0;
	selmon = NULL;
}

int
main(void)
{
	test_layouts();
	test_facts_and_gaps();
	if (failures)
		return EXIT_FAILURE;
	puts("layout regression checks passed");
	return EXIT_SUCCESS;
}
