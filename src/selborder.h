#ifndef DWM_SELBORDER_H
#define DWM_SELBORDER_H

#include <stddef.h>
#include <X11/Xresource.h>

int selborder_load(XrmDatabase db, const char *fallback, char *color, size_t size);

#endif
