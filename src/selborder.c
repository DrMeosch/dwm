#include <errno.h>
#include <string.h>

#include "selborder.h"

int
selborder_load(XrmDatabase db, const char *fallback, char *color, size_t size)
{
	XrmValue value;
	char *type;
	const char *selected = fallback;
	size_t length;
	int overridden = 0;

	if (!fallback || !color || !size) {
		errno = EINVAL;
		return -1;
	}
	if (db && XrmGetResource(db, "dwm.selbordercolor", "Dwm.Selbordercolor", &type, &value)
	&& value.addr && !strcmp(type, "String") && strlen(value.addr) < size) {
		selected = value.addr;
		overridden = 1;
	}
	length = strlen(selected);
	if (length >= size) {
		errno = ERANGE;
		return -1;
	}
	memcpy(color, selected, length + 1);
	return overridden;
}
