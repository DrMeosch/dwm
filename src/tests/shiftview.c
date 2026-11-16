/* Headless checks for shiftview and shifttag circular selection. */
#include <stdio.h>
#include <stdlib.h>

#define LENGTH(X) (sizeof (X) / sizeof (X)[0])
#define SPTAGMASK (1u << 4)

typedef union Arg Arg;
typedef struct Client Client;
typedef struct Monitor Monitor;

union Arg {
	int i;
	unsigned int ui;
};

struct Client {
	unsigned int tags;
	Client *next;
};

struct Monitor {
	unsigned int tagset[2];
	unsigned int seltags;
	Client *clients;
};

static const char *tags[] = {"1", "2", "3", "4"};
static Monitor *selmon;
static unsigned int viewed;
static unsigned int tagged;
static unsigned int view_calls;
static unsigned int tag_calls;

static void
view(const Arg *arg)
{
	viewed = arg->ui;
	view_calls++;
}

static void
tag(const Arg *arg)
{
	tagged = arg->ui;
	tag_calls++;
}

#include "../shiftview.c"

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
test_shiftview(void)
{
	Monitor m = {.tagset = {1, 1}};
	Client occupied = {.tags = 4};
	Client scratch = {.tags = SPTAGMASK | 2, .next = &occupied};
	Arg arg;

	m.clients = &occupied;
	selmon = &m;
	view_calls = 0;
	arg.i = 1;
	shiftview(&arg);
	CHECK(view_calls == 1 && viewed == 4,
	      "shiftview did not skip vacant tags to the right: %u", viewed);

	m.tagset[0] = 1;
	occupied.tags = 8;
	arg.i = -1;
	shiftview(&arg);
	CHECK(viewed == 8, "shiftview did not wrap to the left: %u", viewed);

	m.tagset[0] = SPTAGMASK | 1;
	occupied.tags = 2;
	arg.i = 1;
	shiftview(&arg);
	CHECK(viewed == 2, "shiftview did not strip scratchpad tags: %u", viewed);

	m.clients = NULL;
	m.tagset[0] = 1;
	arg.i = 1;
	shiftview(&arg);
	CHECK(viewed == 2, "shiftview changed an empty view incorrectly: %u", viewed);

	m.tagset[0] = 9;
	shiftview(&arg);
	CHECK(viewed == 3, "shiftview did not rotate a multi-tag view forward: %u", viewed);
	arg.i = -1;
	shiftview(&arg);
	/* view() masks off high bits; the stub records the unmasked argument. */
	CHECK(viewed == 76, "shiftview changed the raw backward multi-tag rotation: %u", viewed);

	m.seltags = 1;
	m.tagset[1] = 1;
	occupied.tags = 4;
	m.clients = &occupied;
	arg.i = 2;
	shiftview(&arg);
	CHECK(viewed == 4, "shiftview did not use the selected tagset and forward step: %u", viewed);
	arg.i = -2;
	shiftview(&arg);
	CHECK(viewed == 4, "shiftview did not use the selected tagset and backward step: %u", viewed);

	m.clients = &scratch;
	occupied.tags = 8;
	arg.i = 1;
	shiftview(&arg);
	CHECK(viewed == 8, "shiftview treated a scratchpad client as an occupied tag: %u", viewed);
	occupied.tags = 4;
	arg.i = -1;
	shiftview(&arg);
	CHECK(viewed == 68, "shiftview changed the raw backward skip of vacant tags: %u", viewed);
}

static void
test_shifttag(void)
{
	Monitor m = {.tagset = {1, 1}};
	Client first = {.tags = 4};
	Client second = {.tags = 8};
	Arg arg = {.i = 1};

	m.clients = &first;
	selmon = &m;
	tag_calls = 0;
	shifttag(&arg);
	CHECK(tag_calls == 1 && tagged == 4,
	      "shifttag did not skip vacant tags: %u", tagged);

	first.tags = 8;
	m.tagset[0] = 1;
	arg.i = -1;
	shifttag(&arg);
	CHECK(tagged == 8, "shifttag did not wrap backwards: %u", tagged);

	first.tags = 8;
	first.next = &second;
	second.tags = 4;
	arg.i = 1;
	shifttag(&arg);
	CHECK(tagged == 4, "shifttag did not consider every client's tags: %u", tagged);

	first.tags = 0;
	arg.i = -1;
	tagged = ~0u;
	shifttag(&arg);
	CHECK(tagged == 4, "shifttag did not skip backward to an occupied tag: %u", tagged);
	m.seltags = 1;
	m.tagset[1] = 1;
	arg.i = 2;
	tagged = ~0u;
	shifttag(&arg);
	CHECK(tagged == 4, "shifttag did not use the selected tagset and step: %u", tagged);
	m.seltags = 0;
	arg.i = 1;

	m.tagset[0] = 3;
	shifttag(&arg);
	CHECK(tagged == 6, "shifttag changed the shifted multi-tag view: %u", tagged);

	m.tagset[0] = 8;
	first.tags = SPTAGMASK;
	second.tags = 0;
	shifttag(&arg);
	CHECK(tagged == (SPTAGMASK | 1), "shifttag ignored a scratchpad tag: %u", tagged);

	m.tagset[0] = 1;
	first.tags = 8;
	arg.i = 0;
	tag_calls = 0;
	shifttag(&arg);
	CHECK(tag_calls == 0, "shifttag tagged when no client matched");
	m.clients = NULL;
	tag_calls = 0;
	shifttag(&arg);
	CHECK(tag_calls == 0, "shifttag tagged when there were no clients");
}

int
main(void)
{
	test_shiftview();
	test_shifttag();
	if (failures)
		return EXIT_FAILURE;
	puts("tag shifting regression checks passed");
	return EXIT_SUCCESS;
}
