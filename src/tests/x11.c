#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

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
pause_millis(long millis)
{
	struct timeval delay = { millis / 1000, (millis % 1000) * 1000 };
	select(0, NULL, NULL, NULL, &delay);
}

static int
viewable(Display *dpy, Window win)
{
	XWindowAttributes wa;
	return XGetWindowAttributes(dpy, win, &wa) && wa.map_state == IsViewable;
}

static Window
find_named_window(Display *dpy, Window root, const char *name)
{
	Window parent, *children = NULL, found = None;
	unsigned int count, i;
	char *title;

	if (!XQueryTree(dpy, root, &root, &parent, &children, &count))
		return None;
	for (i = 0; i < count; i++) {
		if (XFetchName(dpy, children[i], &title) && title) {
			if (!strcmp(title, name))
				found = children[i];
			XFree(title);
		}
		if (found != None)
			break;
	}
	if (children)
		XFree(children);
	return found;
}

static Window
wait_named_window(Display *dpy, Window root, const char *name, int attempts)
{
	Window win = None;
	int i;

	for (i = 0; i < attempts && win == None; i++) {
		win = find_named_window(dpy, root, name);
		if (win == None)
			pause_millis(25);
	}
	return win;
}

static int
wait_viewable(Display *dpy, Window win, int attempts)
{
	int i;

	for (i = 0; i < attempts; i++) {
		if (viewable(dpy, win))
			return 1;
		pause_millis(25);
	}
	return 0;
}

static int
wait_tiled_configure(Display *dpy, Window win, int requested_width,
                     int requested_height, XConfigureEvent *result)
{
	XEvent event;
	int i;

	for (i = 0; i < 100; i++) {
		while (XCheckTypedWindowEvent(dpy, win, ConfigureNotify, &event)) {
			if (event.xconfigure.send_event
			&& (event.xconfigure.width != requested_width
			 || event.xconfigure.height != requested_height)) {
				*result = event.xconfigure;
				return 1;
			}
		}
		pause_millis(25);
	}
	return 0;
}

static int
wait_sized_configure(Display *dpy, Window win, int width, int height)
{
	XEvent event;
	int i;

	for (i = 0; i < 100; i++) {
		while (XCheckTypedWindowEvent(dpy, win, ConfigureNotify, &event))
			if (event.xconfigure.width == width && event.xconfigure.height == height)
				return 1;
		pause_millis(25);
	}
	return 0;
}

static int
check_bar_text(Display *dpy, Window root)
{
	Window parent, *children = NULL, bar = None;
	unsigned int count, i, ink = 0;
	XWindowAttributes wa;
	XImage *image;
	XColor background, exact;
	Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
	int x, y;

	if (!XQueryTree(dpy, root, &root, &parent, &children, &count))
		return 0;
	for (i = 0; i < count; i++) {
		if (XGetWindowAttributes(dpy, children[i], &wa)
		&& wa.override_redirect && wa.map_state == IsViewable
		&& wa.x == 0 && wa.y == 0 && wa.width == DisplayWidth(dpy, DefaultScreen(dpy))
		&& wa.height < 40) {
			bar = children[i];
			break;
		}
	}
	if (children)
		XFree(children);
	if (bar == None || !XAllocNamedColor(dpy, cmap, "#282a36", &background, &exact))
		return 0;
	image = XGetImage(dpy, bar, 0, 0, wa.width, wa.height, AllPlanes, ZPixmap);
	if (!image)
		return 0;
	for (y = 0; y < wa.height; y++)
		for (x = 0; x < wa.width; x++)
			if (XGetPixel(image, x, y) != background.pixel)
				ink++;
	XDestroyImage(image);
	XFreeColors(dpy, cmap, &background.pixel, 1, 0);
	return ink > 10;
}

static int
wait_bar_text(Display *dpy, Window root)
{
	int i;

	for (i = 0; i < 40; i++) {
		if (check_bar_text(dpy, root))
			return 1;
		pause_millis(25);
	}
	return 0;
}

int
main(void)
{
	Display *dpy;
	Window root, tiled, floating, transient, target;
	XWindowAttributes attributes = {0};
	XClassHint class_hint = { "integration", "Integration" };
	XConfigureEvent configure_event;
	int i, got_configure;

	dpy = XOpenDisplay(NULL);
	CHECK(dpy != NULL, "could not open the private X display");
	if (!dpy)
		return EXIT_FAILURE;
	root = DefaultRootWindow(dpy);

	tiled = XCreateSimpleWindow(dpy, root, 40, 40, 400, 300, 0, 0, 0);
	XStoreName(dpy, tiled, "integration-tiled");
	XSetClassHint(dpy, tiled, &class_hint);
	XSelectInput(dpy, tiled, StructureNotifyMask);
	XMapWindow(dpy, tiled);
	XFlush(dpy);
	for (i = 0; i < 40 && !viewable(dpy, tiled); i++)
		pause_millis(25);
	CHECK(viewable(dpy, tiled), "MapRequest was not dispatched for a tiled window");
	if (XGetWindowAttributes(dpy, tiled, &attributes)) {
		CHECK(attributes.border_width == 1,
		      "the selected window did not keep a one-pixel border: %d",
		      attributes.border_width);
		XResizeWindow(dpy, tiled, 111, 123);
		XFlush(dpy);
		got_configure = wait_tiled_configure(dpy, tiled, 111, 123, &configure_event);
		CHECK(got_configure,
		      "tiled ConfigureRequest did not produce a synthetic ConfigureNotify");
		if (got_configure)
			CHECK(configure_event.width != 111 || configure_event.height != 123,
			      "tiled ConfigureRequest changed the window to the requested size");
	} else {
		CHECK(0, "could not inspect the managed tiled window");
	}
	CHECK(wait_bar_text(dpy, root), "dwm did not render text into its bar");

	floating = wait_named_window(dpy, root, "floating", 80);
	CHECK(floating != None, "production transient.c did not create its parent window");
	if (floating != None) {
		int floating_managed = wait_viewable(dpy, floating, 40);
		CHECK(floating_managed, "production parent window was not managed");
		if (floating_managed) {
			XSelectInput(dpy, floating, StructureNotifyMask);
			XResizeWindow(dpy, floating, 230, 180);
			XFlush(dpy);
			CHECK(wait_sized_configure(dpy, floating, 230, 180),
			      "floating ConfigureRequest did not produce ConfigureNotify");
		}
	}

	transient = wait_named_window(dpy, root, "transient", 400);
	CHECK(transient != None, "production transient.c did not create its transient window");
	if (transient != None) {
		int transient_managed = wait_viewable(dpy, transient, 40);
		int related = XGetTransientForHint(dpy, transient, &target) && target == floating;
		CHECK(transient_managed, "production transient window was not managed");
		CHECK(related, "production transient relationship was not retained");
		if (transient_managed && related && XGetWindowAttributes(dpy, transient, &attributes)) {
			CHECK(attributes.border_width == 1,
			      "production transient did not keep a one-pixel border: %d",
			      attributes.border_width);
			CHECK(attributes.x == (DisplayWidth(dpy, DefaultScreen(dpy))
			                         - attributes.width - 2 * attributes.border_width) / 2
			      && attributes.y == (DisplayHeight(dpy, DefaultScreen(dpy))
			                         - attributes.height - 2 * attributes.border_width) / 2,
			      "production transient was not centered: %d,%d", attributes.x, attributes.y);
			XSelectInput(dpy, transient, StructureNotifyMask);
			XResizeWindow(dpy, transient, 137, 119);
			XFlush(dpy);
			CHECK(wait_sized_configure(dpy, transient, 137, 119),
			      "transient ConfigureRequest did not produce ConfigureNotify");
			CHECK(XGetWindowAttributes(dpy, transient, &attributes)
			      && attributes.width == 137 && attributes.height == 119,
			      "transient ConfigureRequest size was not applied");
		} else if (transient_managed && related) {
			CHECK(0, "could not inspect the managed production transient");
		}
	}

	XDestroyWindow(dpy, tiled);
	XFlush(dpy);
	XCloseDisplay(dpy);
	if (failures)
		return EXIT_FAILURE;
	puts("X11 integration checks passed");
	return EXIT_SUCCESS;
}
