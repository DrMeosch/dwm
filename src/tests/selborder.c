#include <stdio.h>
#include <string.h>

#include "../selborder.h"

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
check_resource(const char *resources, const char *expected, int overridden)
{
	XrmDatabase db = resources ? XrmGetStringDatabase(resources) : NULL;
	char color[32] = "unset";
	int result = selborder_load(db, "#ffb86c", color, sizeof color);

	CHECK(result == overridden, "resource override result was %d, expected %d", result, overridden);
	CHECK(strcmp(color, expected) == 0, "selected border color was %s, expected %s", color, expected);
	if (db)
		XrmDestroyDatabase(db);
}

int
main(void)
{
	XrmInitialize();
	check_resource("dwm.selbordercolor: #ffb86c\n", "#ffb86c", 1);
	check_resource("dwm.selbordercolor: #000000\ndwm.color8: #ff00ff\n", "#000000", 1);
	check_resource("dwm.color8: #ff00ff\ndwm.borderpx: 4\n", "#ffb86c", 0);
	check_resource(NULL, "#ffb86c", 0);
	if (failures)
		return 1;
	puts("dedicated selected-border resource checks passed");
	return 0;
}
