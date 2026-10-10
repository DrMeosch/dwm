#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "borderwidth.h"

int
borderwidth_load(XrmDatabase db, unsigned int fallback, unsigned int *width)
{
	XrmValue value;
	char *type, *end;
	unsigned long parsed;

	if (!width || !fallback) {
		errno = EINVAL;
		return -1;
	}
	*width = fallback;
	if (!db || !XrmGetResource(db, "dwm.borderpx", "Dwm.Borderpx", &type, &value)
	|| !value.addr || strcmp(type, "String"))
		return 0;

	errno = 0;
	parsed = strtoul(value.addr, &end, 10);
	if (errno == ERANGE || end == value.addr || *end || !parsed || parsed > USHRT_MAX)
		return 0;

	*width = (unsigned int)parsed;
	return 1;
}
