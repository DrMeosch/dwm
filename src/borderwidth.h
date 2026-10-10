#ifndef DWM_BORDERWIDTH_H
#define DWM_BORDERWIDTH_H

#include <X11/Xresource.h>

int borderwidth_load(XrmDatabase db, unsigned int fallback, unsigned int *width);

#endif
