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

/*
 * Count the bar pixels at or right of from that are color, or, if color is
 * NULL, that differ from the bar background. Reports the leftmost one in left.
 */
static unsigned int
count_bar_pixels(Display *dpy, Window root, const char *color, int from, int *left)
{
	Window bar;
	unsigned int count = 0;
	XWindowAttributes wa;
	XImage *image;
	XColor wanted, exact;
	Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
	int x, y;

	if (left)
		*left = -1;
	if ((bar = find_bar(dpy, root, &wa)) == None
	|| !XAllocNamedColor(dpy, cmap, color ? color : "#282a36", &wanted, &exact))
		return 0;
	image = XGetImage(dpy, bar, 0, 0, wa.width, wa.height, AllPlanes, ZPixmap);
	if (!image)
		return 0;
	for (x = from; x < wa.width; x++)
		for (y = 0; y < wa.height; y++)
			if ((XGetPixel(image, x, y) == wanted.pixel) == (color != NULL)) {
				if (left && *left < 0)
					*left = x;
				count++;
			}
	XDestroyImage(image);
	XFreeColors(dpy, cmap, &wanted.pixel, 1, 0);
	return count;
}

/* The value of one channel of an XImage pixel, given its mask. */
static unsigned long
channel(unsigned long pixel, unsigned long mask)
{
	unsigned long low = mask & -mask;

	return (pixel & mask) / low;
}

/*
 * Count the bar pixels at or right of from that are clearly red: anti-aliased
 * glyphs seldom reach their exact foreground color, but their strong pixels
 * stay near it, and the default status colors never come close.
 */
static unsigned int
count_bar_reddish(Display *dpy, Window root, int from)
{
	Window bar;
	unsigned int count = 0;
	XWindowAttributes wa;
	XImage *image;
	int x, y;

	if ((bar = find_bar(dpy, root, &wa)) == None)
		return 0;
	image = XGetImage(dpy, bar, 0, 0, wa.width, wa.height, AllPlanes, ZPixmap);
	if (!image)
		return 0;
	for (x = from; x < wa.width; x++)
		for (y = 0; y < wa.height; y++) {
			unsigned long p = XGetPixel(image, x, y);

			if (channel(p, image->red_mask) >= 0x80
			&& channel(p, image->green_mask) <= 0x60
			&& channel(p, image->blue_mask) <= 0x60)
				count++;
		}
	XDestroyImage(image);
	return count;
}

static int
check_bar_text(Display *dpy, Window root)
{
	return count_bar_pixels(dpy, root, NULL, 0, NULL) > 10;
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


static void
set_status(Display *dpy, Window root, const char *text)
{
	XStoreName(dpy, root, text);
	XFlush(dpy);
}

/* Wait until the bar, right of from, shows exactly want pixels of color. */
static unsigned int
wait_bar_color(Display *dpy, Window root, const char *color, int from,
               unsigned int want, int *left)
{
	unsigned int count = 0;
	int i;

	for (i = 0; i < 80; i++) {
		count = count_bar_pixels(dpy, root, color, from, left);
		if (count == want)
			break;
		pause_millis(25);
	}
	return count;
}

/*
 * Colored status segments set through the root window name. The walker that
 * draws the status is the one that measures it, so it must end flush against
 * the right edge of the bar however many codes it contains.
 */
static void
check_status_colors(Display *dpy, Window root)
{
	int width = DisplayWidth(dpy, DefaultScreen(dpy));
	int from = width - 400, left = -1, plain_left = -1;
	unsigned int count;

	/* a 4x4 rectangle, 50px of cursor advance, and 2px of padding; ^r does not advance */
	set_status(dpy, root, "^c#ff5555^^r0,0,4,4^^f50^");
	count = wait_bar_color(dpy, root, "#ff5555", from, 16, &left);
	CHECK(count == 16, "^c rectangle drew %u red pixels (expected 16)", count);
	CHECK(left == width - 52, "^f status started at x=%d (expected %d)", left, width - 52);

	/* ^C selects dwm.color<n>, whose defaults are the Dracula palette; ^d resets */
	set_status(dpy, root, "^C2^^r0,0,4,4^^f6^^C5^^r0,0,4,4^^f6^^d^^r0,0,4,4^^f6^");
	CHECK(wait_bar_color(dpy, root, "#50fa7b", from, 16, NULL) == 16,
	      "^C2 rectangle was not drawn in the default palette color");
	CHECK(count_bar_pixels(dpy, root, "#ff79c6", from, NULL) == 32,
	      "^C5 and ^d rectangles were not drawn in the expected colors");

	/* a background and foreground around real glyphs; ^d ends both */
	set_status(dpy, root, "^b#112233^^c#ff2222^WWWW^d^ ^r0,0,4,4^^f6^");
	CHECK(wait_bar_color(dpy, root, "#50fa7b", from, 0, NULL) == 0,
	      "the previous status was not replaced");
	CHECK(count_bar_pixels(dpy, root, "#112233", from, NULL) >= 100,
	      "^b background was not drawn behind text");
	CHECK(count_bar_reddish(dpy, root, from) >= 8,
	      "^c foreground was not used for text");
	CHECK(count_bar_pixels(dpy, root, "#ff79c6", from, NULL) == 16,
	      "^r after ^d was not drawn in the status foreground");

	/* codes take no width: the same text with and without them ends up in the same place */
	set_status(dpy, root, "^c#ff5555^^r0,0,4,4^^f6^WWWW");
	wait_bar_color(dpy, root, "#112233", from, 0, NULL);
	count_bar_pixels(dpy, root, "#ff5555", from, &plain_left);
	set_status(dpy, root, "^c#ff5555^^r0,0,4,4^^f6^^b#112233^^c#ffffff^WWWW^d^");
	CHECK(wait_bar_color(dpy, root, "#ff5555", from, 16, &left) == 16
	      && left == plain_left && left >= 0,
	      "status codes changed the status width: x=%d without, x=%d with", plain_left, left);

	/* malformed codes are literal text: nothing is colored and the bar survives */
	set_status(dpy, root, "^c#ff00^^r0,0,4^^C99^^f12345^");
	CHECK(wait_bar_color(dpy, root, "#ff5555", from, 0, NULL) == 0,
	      "the previous status was not replaced");
	CHECK(count_bar_pixels(dpy, root, NULL, from, NULL) > 50,
	      "a malformed status code was not drawn as text");

	/*
	 * ^f advances add up to far more than the 16 bits an X11 coordinate can
	 * hold, so the start of the status is far off the left edge and runs
	 * drawn there would wrap back into the visible bar. They must not be
	 * drawn: no pixel of the spoofed text or rectangle color may show up.
	 */
	set_status(dpy, root, "^c#12ab34^^r0,0,20,4^SPOOFED-TEXT"
	           "^f9999^^f9999^^f9999^^f9999^^f9999^^f9999^^f6142^"
	           "^d^^C2^^r0,0,4,4^^f6^");
	CHECK(wait_bar_color(dpy, root, "#50fa7b", from, 16, NULL) == 16,
	      "the status with 16-bit overflowing advances was not drawn");
	CHECK(count_bar_pixels(dpy, root, "#12ab34", 0, &left) == 0,
	      "status text or rectangle wrapped around to x=%d of the bar", left);

	/*
	 * The background of a status a little wider than 16 bits must still cover
	 * the whole bar, or pixels from earlier frames are left in it.
	 */
	set_status(dpy, root, "^c#12ab34^^r0,0,20,4^^f600^");
	CHECK(wait_bar_color(dpy, root, "#12ab34", 0, 80, NULL) == 80,
	      "the mid-bar rectangle was not drawn");
	set_status(dpy, root, "^f9999^^f9999^^f9999^^f9999^^f9999^^f9999^^f5600^"
	           "^C2^^r0,0,4,4^^f6^");
	CHECK(wait_bar_color(dpy, root, "#50fa7b", from, 16, NULL) == 16,
	      "the status wider than 16 bits was not drawn");
	CHECK(count_bar_pixels(dpy, root, "#12ab34", 0, &left) == 0,
	      "the status background left stale pixels at x=%d of the bar", left);

	/* left behind for the palette reload check in x11.sh */
	set_status(dpy, root, "^C2^^r0,0,4,4^^f6^^C3^^r0,0,4,4^^f6^");
	CHECK(wait_bar_color(dpy, root, "#50fa7b", from, 16, NULL) == 16,
	      "the palette status was not drawn");
}

int
main(int argc, char *argv[])
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

	/* x11 bar-color <#rrggbb> <pixels>: the status left by a previous run shows that color */
	if (argc == 4 && !strcmp(argv[1], "bar-color")) {
		unsigned int want = (unsigned int)atoi(argv[3]);
		unsigned int count = wait_bar_color(dpy, root, argv[2],
		                                    DisplayWidth(dpy, DefaultScreen(dpy)) - 400, want, NULL);

		CHECK(count == want, "the bar shows %u pixels of %s (expected %u)", count, argv[2], want);
		XCloseDisplay(dpy);
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}

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
	check_status_colors(dpy, root);

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
