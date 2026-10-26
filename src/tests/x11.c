#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
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

static Window
find_bar(Display *dpy, Window root, XWindowAttributes *wa)
{
	Window parent, *children = NULL, bar = None;
	unsigned int count, i;

	if (!XQueryTree(dpy, root, &root, &parent, &children, &count))
		return None;
	for (i = 0; i < count; i++) {
		if (XGetWindowAttributes(dpy, children[i], wa)
		&& wa->override_redirect && wa->map_state == IsViewable
		&& wa->x == 0 && wa->y == 0 && wa->width == DisplayWidth(dpy, DefaultScreen(dpy))
		&& wa->height < 40) {
			bar = children[i];
			break;
		}
	}
	if (children)
		XFree(children);
	return bar;
}

static int
check_bar_text(Display *dpy, Window root)
{
	Window bar;
	unsigned int ink = 0;
	XWindowAttributes wa;
	XImage *image;
	XColor background, exact;
	Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
	int x, y;

	bar = find_bar(dpy, root, &wa);
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

/* Longest horizontal run of non-background pixels; returns its start column. */
static int
longest_ink_run(XImage *image, unsigned long background, int *start)
{
	int x, y, run, best = 0;

	*start = 0;
	for (y = 0; y < image->height; y++) {
		for (x = 0, run = 0; x <= image->width; x++) {
			if (x < image->width && XGetPixel(image, x, y) != background) {
				run++;
				continue;
			}
			if (run > best) {
				best = run;
				*start = x - run;
			}
			run = 0;
		}
	}
	return best;
}

/* Count non-background pixels in columns [from, to) of the top rows. */
static int
ink_in_columns(XImage *image, unsigned long background, int from, int to,
               int rows)
{
	int x, y, ink = 0;

	for (y = 0; y < rows; y++)
		for (x = from; x < to; x++)
			if (XGetPixel(image, x, y) != background)
				ink++;
	return ink;
}

static XImage *
bar_image(Display *dpy, Window bar, const XWindowAttributes *wa)
{
	return XGetImage(dpy, bar, 0, 0, wa->width, wa->height, AllPlanes, ZPixmap);
}

/*
 * The title is a long underscore line, so it shows as one wide run of ink that
 * starts lrpad / 2 pixels after the layout symbol. For each status width that
 * leaves a narrow-but-positive gap between the layout symbol and the status,
 * go there directly from a short status that showed the full title. The title
 * is not drawn in such a gap, and the strip where it was must be cleared
 * rather than keep showing the start of the old title.
 */
static void
check_narrow_title_gap(Display *dpy, Window root, Window client)
{
	Window bar;
	XWindowAttributes wa;
	XColor background, exact;
	Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
	XftFont *font;
	XGlyphInfo extents;
	XImage *image;
	char title[128], status[256];
	int k, i, h, bh, title_start = 0, run = 0, x, tw, gap, cell;
	int narrow = 0, stale = 0, stale_k = 0, ink;

	bar = find_bar(dpy, root, &wa);
	font = XftFontOpenName(dpy, DefaultScreen(dpy), "monospace:size=10");
	CHECK(bar != None && font != NULL
	      && XAllocNamedColor(dpy, cmap, "#282a36", &background, &exact),
	      "could not prepare the narrow title gap check");
	if (bar == None || !font)
		return;
	h = font->ascent + font->descent; /* dwm: lrpad = fonts->h, bh = h + 2 */
	bh = h + 2;

	memset(title, '_', sizeof title - 1);
	title[sizeof title - 1] = '\0';
	XStoreName(dpy, client, title);
	XStoreName(dpy, root, "W");
	XFlush(dpy);
	for (i = 0; i < 40; i++) {
		image = bar_image(dpy, bar, &wa);
		run = image ? longest_ink_run(image, background.pixel, &title_start) : 0;
		if (image)
			XDestroyImage(image);
		if (run > 40)
			break;
		pause_millis(25);
	}
	CHECK(run > 40, "the selected client title was not drawn in the bar");
	x = title_start - h / 2; /* end of the layout symbol */

	for (k = 2; run > 40 && k < (int)sizeof status; k++) {
		memset(status, 'W', k);
		status[k] = '\0';
		XftTextExtentsUtf8(dpy, font, (const FcChar8 *)status, k, &extents);
		tw = extents.xOff + 2; /* dwm adds 2px right padding */
		gap = wa.width - tw - x;
		/* some title ink must fit in the gap; the title is hidden up to bh */
		if (gap <= h / 2 || gap > bh - 2)
			continue;
		narrow++;
		cell = extents.xOff / k;

		XStoreName(dpy, root, "W");
		XFlush(dpy);
		for (i = 0; i < 40; i++) {
			image = bar_image(dpy, bar, &wa);
			ink = image ? ink_in_columns(image, background.pixel, x + h / 2,
			                             wa.width - 10 - cell, wa.height) : 0;
			if (image)
				XDestroyImage(image);
			if (ink > 100)
				break;
			pause_millis(25);
		}
		XStoreName(dpy, root, status);
		XFlush(dpy);
		/* wait for the first status cell, blank in the short status; the title's
		 * underscores sit in the bottom rows, so only look at the top half */
		for (i = 0, image = NULL; i < 40; i++) {
			image = bar_image(dpy, bar, &wa);
			ink = image ? ink_in_columns(image, background.pixel,
			                             wa.width - tw, wa.width - tw + cell,
			                             wa.height / 2) : 0;
			if (ink || i == 39)
				break;
			if (image)
				XDestroyImage(image);
			pause_millis(25);
		}
		if (!image)
			continue;
		ink = ink_in_columns(image, background.pixel, x, wa.width - tw,
		                     wa.height);
		if (ink) {
			stale = ink;
			stale_k = k;
		}
		XDestroyImage(image);
	}
	CHECK(narrow > 0, "no status width left a narrow gap before the status");
	CHECK(stale == 0,
	      "a %d-character status left %d stale title pixels between the layout symbol and the status",
	      stale_k, stale);

	XStoreName(dpy, client, "integration-tiled");
	XStoreName(dpy, root, "dwm-test");
	XftFontClose(dpy, font);
	XFlush(dpy);
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
		      "the selected dark-theme window did not keep a one-pixel border: %d",
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
	check_narrow_title_gap(dpy, root, tiled);

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
			      "production dark-theme transient did not keep a one-pixel border: %d",
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
