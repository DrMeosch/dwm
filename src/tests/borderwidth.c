#include <stdio.h>
#include <string.h>

#include "../borderwidth.h"

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
check_resource(const char *resources, unsigned int expected, int overridden)
{
	XrmDatabase db = resources ? XrmGetStringDatabase(resources) : NULL;
	unsigned int width = 99;
	int result = borderwidth_load(db, 2, &width);

	CHECK(result == overridden, "resource override result was %d, expected %d",
	      result, overridden);
	CHECK(width == expected, "border width was %u, expected %u", width, expected);
	if (db)
		XrmDestroyDatabase(db);
}

int
main(void)
{
	unsigned int width = 99;
	XrmDatabase db;

	XrmInitialize();
	check_resource("dwm.borderpx: 1\n", 1, 1);
	check_resource("dwm.borderpx: 1\ndwm.color8: 9\n", 1, 1);
	check_resource("dwm.borderpx: 2\n", 2, 1);
	check_resource("dwm.color8: 1\n", 2, 0);
	check_resource("dwmblocks.borderpx: 1\n", 2, 0);
	check_resource("dwm.borderpx: invalid\n", 2, 0);
	check_resource("dwm.borderpx: 0\n", 2, 0);
	check_resource("dwm.borderpx: -1\n", 2, 0);
	check_resource("dwm.borderpx: 65536\n", 2, 0);
	check_resource(NULL, 2, 0);

	db = XrmGetStringDatabase("dwm.borderpx: 1\n");
	width = 2;
	CHECK(borderwidth_load(db, 2, NULL) == -1,
	      "a null output pointer was accepted");
	CHECK(borderwidth_load(db, 0, &width) == -1,
	      "a zero fallback was accepted");
	CHECK(width == 2, "invalid arguments changed the output width to %u", width);
	XrmDestroyDatabase(db);

	if (failures)
		return 1;
	puts("dedicated border-width resource checks passed");
	return 0;
}
