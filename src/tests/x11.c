#include <X11/Xatom.h>
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
	int from = width - 400, left = -1, plain_left = -1, fallback_left, i;
	unsigned int count, fallback_count;

	/* The test dwm is built with VERSION=6.5. Deleting the name restores that fallback. */
	set_status(dpy, root, "^c#ff5555^^r0,0,4,4^^f50^");
	CHECK(wait_bar_color(dpy, root, "#ff5555", from, 16, NULL) == 16,
	      "the status before the fallback was not drawn");
	set_status(dpy, root, "dwm-6.5");
	CHECK(wait_bar_color(dpy, root, "#ff5555", from, 0, NULL) == 0,
	      "the explicit fallback status was not drawn");
	fallback_count = count_bar_pixels(dpy, root, NULL, width - 100, &fallback_left);
	CHECK(fallback_count > 0, "the explicit fallback status was empty");
	set_status(dpy, root, "^c#ff5555^^r0,0,4,4^^f50^");
	CHECK(wait_bar_color(dpy, root, "#ff5555", from, 16, NULL) == 16,
	      "the status before deleting the root name was not drawn");
	XDeleteProperty(dpy, root, XA_WM_NAME);
	XFlush(dpy);
	for (i = 0; i < 80; i++) {
		count = count_bar_pixels(dpy, root, NULL, width - 100, &left);
		if (count == fallback_count && left == fallback_left)
			break;
		pause_millis(25);
	}
	CHECK(i < 80, "deleting the root name did not restore the fallback status");

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

/*
 * Map a window whose _NET_WM_STATE is set with the given format and data, and
 * report whether dwm made it fullscreen: it covers the screen, and dwm wrote
 * the state back as a 32-bit ATOM.
 */
static int
mapped_fullscreen(Display *dpy, Window root, const char *name, int format,
                  const unsigned char *data, int n)
{
	Atom state = XInternAtom(dpy, "_NET_WM_STATE", False), type;
	XWindowAttributes wa;
	Window win;
	unsigned long nitems, after;
	unsigned char *p = NULL;
	int actual = 0, full;

	win = XCreateSimpleWindow(dpy, root, 40, 40, 300, 200, 0, 0, 0);
	XStoreName(dpy, win, name);
	XChangeProperty(dpy, win, state, XA_ATOM, format, PropModeReplace, data, n);
	XMapWindow(dpy, win);
	XFlush(dpy);
	/* dwm reads the state before it maps the window */
	CHECK(wait_viewable(dpy, win, 40), "MapRequest was not dispatched for %s", name);
	full = XGetWindowAttributes(dpy, win, &wa)
	    && wa.x == 0 && wa.y == 0 && wa.border_width == 0
	    && wa.width == DisplayWidth(dpy, DefaultScreen(dpy))
	    && wa.height == DisplayHeight(dpy, DefaultScreen(dpy));
	if (XGetWindowProperty(dpy, win, state, 0, 1, False, XA_ATOM, &type, &actual,
	                       &nitems, &after, &p) == Success && p)
		XFree(p);
	XDestroyWindow(dpy, win);
	XSync(dpy, False);
	return full && actual == 32;
}

/*
 * dwm reads _NET_WM_STATE and _NET_WM_WINDOW_TYPE as one 32-bit ATOM, which a
 * client may set with another format: 8-bit data holding the bytes of an atom
 * must not be taken for it, and a single byte must not be read past.
 */
static void
check_atom_format(Display *dpy, Window root)
{
	Atom fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	unsigned char bytes[sizeof fullscreen], one = 0;

	CHECK(mapped_fullscreen(dpy, root, "atom-format-32", 32,
	                        (unsigned char *)&fullscreen, 1),
	      "a 32-bit _NET_WM_STATE_FULLSCREEN did not make the window fullscreen");
	memcpy(bytes, &fullscreen, sizeof bytes);
	CHECK(!mapped_fullscreen(dpy, root, "atom-format-8", 8, bytes, sizeof bytes),
	      "the bytes of _NET_WM_STATE_FULLSCREEN in 8-bit data were taken for the atom");
	CHECK(!mapped_fullscreen(dpy, root, "atom-format-8-short", 8, &one, 1),
	      "a one-byte 8-bit _NET_WM_STATE made the window fullscreen");
}

/*
 * Map a window whose _NET_CLIENT_INFO is set with the given format and data,
 * and return the tags dwm wrote back when it managed the window, or 0 when it
 * did not write them back as two 32-bit CARDINALs. dwm keeps windows on hidden
 * tags mapped but off the left edge, so shown means on the screen.
 */
static unsigned long
mapped_client_tags(Display *dpy, Window root, const char *name, int format,
                   const unsigned char *data, int n, int *shown)
{
	Atom info = XInternAtom(dpy, "_NET_CLIENT_INFO", False), type;
	XWindowAttributes wa;
	Window win;
	unsigned long nitems, after, tags = 0;
	unsigned char *p = NULL;
	int actual = 0, i;

	win = XCreateSimpleWindow(dpy, root, 40, 40, 300, 200, 0, 0, 0);
	XStoreName(dpy, win, name);
	XChangeProperty(dpy, win, info, XA_CARDINAL, format, PropModeReplace, data, n);
	XMapWindow(dpy, win);
	XFlush(dpy);
	/* dwm writes the tags back before it maps the window */
	CHECK(wait_viewable(dpy, win, 40), "MapRequest was not dispatched for %s", name);
	if (XGetWindowProperty(dpy, win, info, 0, 2, False, XA_CARDINAL, &type, &actual,
	                       &nitems, &after, &p) == Success && p) {
		if (actual == 32 && nitems == 2)
			tags = ((unsigned long *)p)[0];
		XFree(p);
	}
	*shown = 0;
	for (i = 0; i < 20 && !*shown; i++) {
		*shown = XGetWindowAttributes(dpy, win, &wa) && wa.x >= 0
		      && wa.x < DisplayWidth(dpy, DefaultScreen(dpy));
		if (!*shown)
			pause_millis(25);
	}
	XDestroyWindow(dpy, win);
	XSync(dpy, False);
	return tags;
}

/*
 * dwm restores tags and monitor from _NET_CLIENT_INFO, which a client may set
 * before it maps: only two 32-bit CARDINALs are read, and tags outside the
 * tag mask are dropped, so the window still lands on a tag.
 */
static void
check_client_info(Display *dpy, Window root)
{
	unsigned char bytes[2] = { 0, 0 };
	long outside[2] = { 1L << 20, 0 }, second[2] = { 1L << 1, 0 };
	unsigned long tags;
	int shown;

	tags = mapped_client_tags(dpy, root, "client-info-8", 8, bytes, 2, &shown);
	CHECK(shown && tags, "an 8-bit _NET_CLIENT_INFO left the window on tags %#lx%s",
	      tags, shown ? "" : ", not shown");
	tags = mapped_client_tags(dpy, root, "client-info-outside", 32,
	                          (unsigned char *)outside, 2, &shown);
	CHECK(shown && tags && !(tags & outside[0]),
	      "_NET_CLIENT_INFO tags outside the tag mask left the window on tags %#lx%s",
	      tags, shown ? "" : ", not shown");
	tags = mapped_client_tags(dpy, root, "client-info-32", 32,
	                          (unsigned char *)second, 2, &shown);
	CHECK(tags == (unsigned long)second[0],
	      "a 32-bit _NET_CLIENT_INFO on tag 2 was restored as tags %#lx", tags);
}

/*
 * Geometry a client controls must not break dwm: negative size hints on a
 * floating window are no limit, and a ConfigureNotify for the root window
 * with no size, which any client can send, is ignored.
 */
static void
check_malformed_geometry(Display *dpy, Window root)
{
	Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	Atom dialog = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	XSizeHints *hints = XAllocSizeHints();
	XWindowAttributes wa = {0}, bar;
	XEvent ev;
	Window win;

	win = XCreateSimpleWindow(dpy, root, 40, 40, 300, 200, 0, 0, 0);
	XStoreName(dpy, win, "negative-size-hints");
	XChangeProperty(dpy, win, type, XA_ATOM, 32, PropModeReplace,
	                (unsigned char *)&dialog, 1);
	if (hints) {
		hints->flags = PMinSize | PMaxSize | PResizeInc | PAspect;
		hints->min_width = hints->min_height = -40;
		hints->max_width = hints->max_height = -5;
		hints->width_inc = hints->height_inc = -3;
		hints->min_aspect.x = hints->min_aspect.y = 0;
		hints->max_aspect.x = hints->max_aspect.y = 0;
		XSetWMNormalHints(dpy, win, hints);
		XFree(hints);
	}
	XMapWindow(dpy, win);
	XFlush(dpy);
	CHECK(wait_viewable(dpy, win, 40), "a window with negative size hints was not managed");
	CHECK(XGetWindowAttributes(dpy, win, &wa) && wa.width == 300 && wa.height == 200,
	      "negative size hints resized a floating window to %dx%d", wa.width, wa.height);
	XDestroyWindow(dpy, win);
	XSync(dpy, False);

	memset(&ev, 0, sizeof ev);
	ev.xconfigure.type = ConfigureNotify;
	ev.xconfigure.event = ev.xconfigure.window = root;
	XSendEvent(dpy, root, False, StructureNotifyMask, &ev);
	XSync(dpy, False);
	pause_millis(200);
	CHECK(find_bar(dpy, root, &bar) != None,
	      "a root ConfigureNotify without a size changed the bar");
}

/* Wait for both the advertised client list and focus to reflect a removal. */
static int
wait_clients(Display *dpy, Window root, const Window *wins, unsigned int count, Window focus)
{
	Atom property = XInternAtom(dpy, "_NET_CLIENT_LIST", False), type;
	unsigned char *data;
	unsigned long n, after;
	Window selected;
	int format, revert, i, matches;
	unsigned int j;

	for (i = 0; i < 100; i++) {
		data = NULL;
		matches = XGetWindowProperty(dpy, root, property, 0, 128, False, XA_WINDOW,
		                             &type, &format, &n, &after, &data) == Success
		       && n == count && !after && (count == 0 || (type == XA_WINDOW && format == 32));
		for (j = 0; matches && j < count; j++)
			matches = ((Window *)data)[j] == wins[j];
		if (data)
			XFree(data);
		XGetInputFocus(dpy, &selected, &revert);
		if (matches && selected == focus)
			return 1;
		pause_millis(25);
	}
	return 0;
}

/* A marked tiled reply orders focus assertions after key and property events. */
static void
focus_roundtrip(Display *dpy, Window barrier, const char *step)
{
	static int marker = 1;
	XEvent ev;
	int i, replied = 0;

	while (XCheckTypedWindowEvent(dpy, barrier, ConfigureNotify, &ev))
		;
	XSetWindowBorderWidth(dpy, barrier, ++marker);
	XResizeWindow(dpy, barrier, 17, 19);
	XFlush(dpy);
	for (i = 0; i < 100 && !replied; i++) {
		while (XCheckTypedWindowEvent(dpy, barrier, ConfigureNotify, &ev))
			if (ev.xconfigure.send_event && ev.xconfigure.border_width == marker)
				replied = 1;
		if (!replied)
			pause_millis(25);
	}
	CHECK(replied, "%s: dwm did not complete the focus round trip", step);
}

/* A tiled ConfigureRequest is a dwm round trip after the preceding message,
 * including requests that should be ignored and therefore produce no event. */
static void
fullscreen_request(Display *dpy, Window root, Window win, Window barrier,
                   long action, Atom first, Atom second)
{
	XEvent ev = {0};
	XConfigureEvent reply;

	while (XCheckTypedWindowEvent(dpy, barrier, ConfigureNotify, &ev))
		;
	memset(&ev, 0, sizeof ev);
	ev.xclient.type = ClientMessage;
	ev.xclient.window = win;
	ev.xclient.message_type = XInternAtom(dpy, "_NET_WM_STATE", False);
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = action;
	ev.xclient.data.l[1] = first;
	ev.xclient.data.l[2] = second;
	CHECK(XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev),
	      "could not send fullscreen action %ld", action);
	XResizeWindow(dpy, barrier, 17, 19);
	XFlush(dpy);
	CHECK(wait_tiled_configure(dpy, barrier, 17, 19, &reply),
	      "dwm did not complete the fullscreen action %ld round trip", action);
}

static void
check_fullscreen_state(Display *dpy, Window win, const XWindowAttributes *normal,
                       int full, const char *kind, const char *step)
{
	Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
	Atom fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False), type;
	XWindowAttributes wa = {0};
	unsigned char *data = NULL;
	unsigned long n = 0, after;
	int format = 0, present = 0;

	CHECK(XGetWindowProperty(dpy, win, state, 0, 16, False, XA_ATOM,
	                         &type, &format, &n, &after, &data) == Success,
	      "%s %s: could not read fullscreen state", kind, step);
	if (data) {
		unsigned long i;

		for (i = 0; type == XA_ATOM && format == 32 && i < n; i++)
			if (((Atom *)data)[i] == fullscreen)
				present = 1;
		XFree(data);
	}
	CHECK(present == full, "%s %s: fullscreen property is %d, expected %d",
	      kind, step, present, full);
	CHECK(XGetWindowAttributes(dpy, win, &wa), "%s %s: cannot inspect geometry", kind, step);
	CHECK(wa.x == (full ? 0 : normal->x) && wa.y == (full ? 0 : normal->y)
	      && wa.width == (full ? DisplayWidth(dpy, DefaultScreen(dpy)) : normal->width)
	      && wa.height == (full ? DisplayHeight(dpy, DefaultScreen(dpy)) : normal->height)
	      && wa.border_width == (full ? 0 : normal->border_width),
	      "%s %s: unexpected geometry (%d,%d %dx%d border %d)",
	      kind, step, wa.x, wa.y, wa.width, wa.height, wa.border_width);
}

static void
check_fullscreen_messages(Display *dpy, Window root)
{
	static const struct { long action; int second, full; const char *step; } steps[] = {
		{ 0, 0, 0, "remove while normal" },
		{ 1, 0, 1, "add" },
		{ 1, 0, 1, "repeated add" },
		{ 3, 0, 1, "invalid action 3 must be ignored" },
		{ 1, 0, 1, "add after invalid action" },
		{ -1, 0, 1, "invalid action -1 must be ignored" },
		{ 1, 0, 1, "add after negative action" },
		{ 2, 0, 0, "toggle off" },
		{ 2, 1, 1, "toggle on using second atom" },
		{ 0, 1, 0, "remove using second atom" },
		{ 0, 0, 0, "repeated remove" },
	};
	Atom fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	Atom dialog = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	Atom unrelated = XInternAtom(dpy, "_DWM_TEST_UNRELATED_STATE", False);
	Window barrier, win, pair[2], focused;
	XWindowAttributes normal = {0};
	int floating, revert;
	size_t i;

	for (floating = 0; floating < 2; floating++) {
		const char *kind = floating ? "floating" : "tiled";

		barrier = XCreateSimpleWindow(dpy, root, 40, 40, 200, 150, 0, 0, 0);
		XSelectInput(dpy, barrier, StructureNotifyMask);
		XMapWindow(dpy, barrier);
		XFlush(dpy);
		CHECK(wait_clients(dpy, root, &barrier, 1, barrier), "fullscreen barrier was not managed");
		win = XCreateSimpleWindow(dpy, root, 80, 90, 300, 200, 0, 0, 0);
		XStoreName(dpy, win, "fullscreen-messages");
		XSelectInput(dpy, win, StructureNotifyMask);
		if (floating)
			XChangeProperty(dpy, win, type, XA_ATOM, 32, PropModeReplace,
			                (unsigned char *)&dialog, 1);
		XMapWindow(dpy, win);
		XFlush(dpy);
		pair[0] = barrier; pair[1] = win;
		CHECK(wait_clients(dpy, root, pair, 2, win), "%s fullscreen target was not managed", kind);
		fullscreen_request(dpy, root, win, barrier, 1, unrelated, None);
		CHECK(XGetWindowAttributes(dpy, win, &normal), "cannot inspect %s initial geometry", kind);
		check_fullscreen_state(dpy, win, &normal, 0, kind, "unrelated state atom");
		for (i = 0; i < sizeof steps / sizeof steps[0]; i++) {
			fullscreen_request(dpy, root, win, barrier, steps[i].action,
			                   steps[i].second ? unrelated : fullscreen,
			                   steps[i].second ? fullscreen : None);
			check_fullscreen_state(dpy, win, &normal, steps[i].full, kind, steps[i].step);
			XGetInputFocus(dpy, &focused, &revert);
			CHECK(focused == win, "%s %s: request changed focus", kind, steps[i].step);
		}
		XResizeWindow(dpy, win, 137, 119);
		XFlush(dpy);
		if (floating)
			CHECK(wait_sized_configure(dpy, win, 137, 119),
			      "fullscreen exit did not restore floating resize behavior");
		else {
			XConfigureEvent reply;

			CHECK(wait_tiled_configure(dpy, win, 137, 119, &reply),
			      "fullscreen exit did not restore tiled resize behavior");
			check_fullscreen_state(dpy, win, &normal, 0, kind, "resize after exit");
		}
		XDestroyWindow(dpy, win);
		XDestroyWindow(dpy, barrier);
		XFlush(dpy);
		CHECK(wait_clients(dpy, root, NULL, 0, root), "fullscreen fixture did not clean up its clients");
	}
}

/* Malformed formats carry matching bytes, but only format 32 is an EWMH request. */
static void
check_fullscreen_message_formats(Display *dpy, Window root)
{
	static const struct { long action; int initial; const char *name; } cases[] = {
		{ 1, 0, "ADD while normal" },
		{ 1, 1, "ADD while fullscreen" },
		{ 0, 0, "REMOVE while normal" },
		{ 0, 1, "REMOVE while fullscreen" },
		{ 2, 0, "TOGGLE while normal" },
		{ 2, 1, "TOGGLE while fullscreen" },
	};
	static const int formats[] = { 8, 16, 32 };
	Atom fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
	Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	Atom dialog = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	Atom unrelated = XInternAtom(dpy, "_DWM_TEST_UNRELATED_STATE", False);
	Window barrier, win, pair[2], focused;
	XWindowAttributes normal = {0};
	XEvent ev;
	char step[128];
	long payload[5];
	int floating, second, revert, expected;
	size_t i, j;

	XWarpPointer(dpy, None, root, 0, 0, 0, 0, 1100, 5);
	for (floating = 0; floating < 2; floating++) {
		const char *kind = floating ? "floating" : "tiled";

		barrier = XCreateSimpleWindow(dpy, root, 40, 40, 200, 150, 0, 0, 0);
		XSelectInput(dpy, barrier, StructureNotifyMask);
		XMapWindow(dpy, barrier);
		XFlush(dpy);
		CHECK(wait_clients(dpy, root, &barrier, 1, barrier), "message formats barrier was not managed");
		win = XCreateSimpleWindow(dpy, root, 80, 90, 300, 200, 0, 0, 0);
		XStoreName(dpy, win, "fullscreen-message-formats");
		if (floating)
			XChangeProperty(dpy, win, type, XA_ATOM, 32, PropModeReplace,
			                (unsigned char *)&dialog, 1);
		XMapWindow(dpy, win);
		XFlush(dpy);
		pair[0] = barrier; pair[1] = win;
		CHECK(wait_clients(dpy, root, pair, 2, win), "%s message formats target was not managed", kind);
		focus_roundtrip(dpy, barrier, "message formats mapping");
		CHECK(XGetWindowAttributes(dpy, win, &normal), "cannot inspect %s initial geometry", kind);
		for (j = 0; j < sizeof formats / sizeof formats[0]; j++)
			for (second = 0; second < 2; second++)
				for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
					snprintf(step, sizeof step, "format %d slot %d %s", formats[j], second + 1, cases[i].name);
					/* Reset through valid requests so failures cannot contaminate later cases. */
					fullscreen_request(dpy, root, win, barrier, 0, fullscreen, None);
					if (cases[i].initial)
						fullscreen_request(dpy, root, win, barrier, 1, fullscreen, None);
					check_fullscreen_state(dpy, win, &normal, cases[i].initial, kind, "reset control");
					memset(&ev, 0, sizeof ev);
					ev.xclient.type = ClientMessage;
					ev.xclient.window = win;
					ev.xclient.message_type = state;
					ev.xclient.format = formats[j];
					payload[0] = cases[i].action;
					payload[1] = second ? unrelated : fullscreen;
					payload[2] = second ? fullscreen : None;
					payload[3] = payload[4] = 0;
					/* For 8/16, serialize 20 bytes, not 32-bit EWMH fields. These
					 * deliberately resemble data.l to catch unchecked union reads. */
					if (formats[j] == 32)
						memcpy(ev.xclient.data.l, payload, sizeof payload);
					else
						memcpy(ev.xclient.data.b, payload, sizeof ev.xclient.data.b);
					CHECK(XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev),
					      "%s %s: could not send client message", kind, step);
					focus_roundtrip(dpy, barrier, step);
					expected = formats[j] != 32 ? cases[i].initial
					         : cases[i].action == 1 || (cases[i].action == 2 && !cases[i].initial);
					check_fullscreen_state(dpy, win, &normal, expected, kind, step);
					XGetInputFocus(dpy, &focused, &revert);
					CHECK(focused == win, "%s %s: message changed focus", kind, step);
				}
		XDestroyWindow(dpy, win);
		XDestroyWindow(dpy, barrier);
		XFlush(dpy);
		CHECK(wait_clients(dpy, root, NULL, 0, root), "message formats fixture did not clean up its clients");
	}
}

/* Atom order is not significant, but missing, extra and duplicate atoms are. */
static void
check_state_atoms(Display *dpy, Window win, const Atom *expected, unsigned long count,
                  const char *kind, const char *step)
{
	Atom type;
	unsigned char *data = NULL;
	unsigned long n = 0, after = 0, i, j;
	int format = 0, matches;

	matches = XGetWindowProperty(dpy, win, XInternAtom(dpy, "_NET_WM_STATE", False),
	                             0, 1024, False, XA_ATOM, &type, &format, &n,
	                             &after, &data) == Success
	       && type == XA_ATOM && format == 32 && !after && n == count;
	for (i = 0; matches && i < count; i++) {
		unsigned int occurrences = 0;

		for (j = 0; j < n; j++)
			occurrences += ((Atom *)data)[j] == expected[i];
		matches = occurrences == 1;
	}
	CHECK(matches, "%s %s: _NET_WM_STATE has %lu atoms, expected exactly %lu",
	      kind, step, n, count);
	if (data)
		XFree(data);
}

/* Compare pre-map fullscreen with the same client's ordinary map/exit path.
 * Floating positions may differ, but size, border and floating behavior must not. */
static void
check_initial_fullscreen_exit(Display *dpy, Window root)
{
	static const char *kinds[] = { "tiled", "dialog", "transient", "rule-floating" };
	Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
	Atom fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	Atom dialog = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	Window barrier, win, pair[2], focused;
	XWindowAttributes normal = {0}, wa = {0};
	int kind, initial, action, revert;
	char step[128];

	XWarpPointer(dpy, None, root, 0, 0, 0, 0, 1, 1);
	for (kind = 0; kind < 4; kind++) {
		for (action = 0; action <= 2; action += 2) {
			barrier = XCreateSimpleWindow(dpy, root, 40, 40, 200, 150, 0, 0, 0);
			XSelectInput(dpy, barrier, StructureNotifyMask);
			XMapWindow(dpy, barrier);
			XFlush(dpy);
			CHECK(wait_clients(dpy, root, &barrier, 1, barrier), "initial exit barrier was not managed");
			for (initial = 0; initial < 2; initial++) {
				snprintf(step, sizeof step, "%s %s action %d", kinds[kind],
				         initial ? "initial fullscreen" : "mapped-normal control", action);
				win = XCreateSimpleWindow(dpy, root, 80, 90, 300, 200, 0, 0, 0);
				XStoreName(dpy, win, step);
				if (kind == 1)
					XChangeProperty(dpy, win, type, XA_ATOM, 32, PropModeReplace,
					                (unsigned char *)&dialog, 1);
				if (kind == 2)
					XSetTransientForHint(dpy, win, barrier);
				if (kind == 3) {
					XClassHint cls = { "initial-exit", "ibcalpha-ibc-IbcTws" };

					XSetClassHint(dpy, win, &cls);
				}
				if (initial)
					XChangeProperty(dpy, win, state, XA_ATOM, 32, PropModeReplace,
					                (unsigned char *)&fullscreen, 1);
				XMapWindow(dpy, win);
				XFlush(dpy);
				pair[0] = barrier; pair[1] = win;
				CHECK(wait_clients(dpy, root, pair, 2, win), "%s: target was not managed", step);
				focus_roundtrip(dpy, barrier, step);
				if (!initial) {
					CHECK(XGetWindowAttributes(dpy, win, &normal), "%s: cannot inspect control", step);
					CHECK(normal.border_width > 0 && (!kind || (normal.width == 300 && normal.height == 200)),
					      "%s: invalid normal geometry control", step);
				} else
					check_fullscreen_state(dpy, win, &normal, 1, step, "on mapping");
				/* ADD is also an idempotence check for pre-map fullscreen. */
				fullscreen_request(dpy, root, win, barrier, 1, fullscreen, None);
				check_fullscreen_state(dpy, win, &normal, 1, step, "after add");
				fullscreen_request(dpy, root, win, barrier, action, fullscreen, None);
				check_state_atoms(dpy, win, NULL, 0, step, "after exit");
				CHECK(XGetWindowAttributes(dpy, win, &wa), "%s: cannot inspect exit geometry", step);
				CHECK(wa.width == normal.width && wa.height == normal.height
				      && wa.border_width == normal.border_width
				      && (kind || (wa.x == normal.x && wa.y == normal.y)),
				      "%s: exit geometry (%d,%d %dx%d border %d), normal (%d,%d %dx%d border %d)",
				      step, wa.x, wa.y, wa.width, wa.height, wa.border_width,
				      normal.x, normal.y, normal.width, normal.height, normal.border_width);
				XResizeWindow(dpy, win, 137, 119);
				focus_roundtrip(dpy, barrier, step);
				CHECK(XGetWindowAttributes(dpy, win, &wa), "%s: cannot inspect resize", step);
				CHECK(wa.width == (kind ? 137 : normal.width) && wa.height == (kind ? 119 : normal.height),
				      "%s: resize after exit is %dx%d, expected %dx%d", step,
				      wa.width, wa.height, kind ? 137 : normal.width, kind ? 119 : normal.height);
				XGetInputFocus(dpy, &focused, &revert);
				CHECK(focused == win, "%s: exit/resize changed focus", step);
				XDestroyWindow(dpy, win);
				XFlush(dpy);
				CHECK(wait_clients(dpy, root, &barrier, 1, barrier), "%s: target was not removed", step);
			}
			XDestroyWindow(dpy, barrier);
			XFlush(dpy);
			CHECK(wait_clients(dpy, root, NULL, 0, root), "initial exit fixture did not clean up its clients");
		}
	}
}

/* Entering and leaving fullscreen changes only the fullscreen atom. The other
 * states are set before mapping, as clients do, and never rewritten by the test,
 * so each list is what dwm left after the real protocol transitions. */
static void
check_fullscreen_state_preservation(Display *dpy, Window root)
{
	static const struct { long action; int target, second, full; const char *step; } steps[] = {
		{ 1, 0, 0, 0, "unrelated request while normal" },
		{ 1, 1, 0, 1, "add" },
		{ 1, 1, 0, 1, "repeated add" },
		{ 0, 0, 0, 1, "unrelated request while fullscreen" },
		{ 0, 1, 0, 0, "remove" },
		{ 0, 1, 0, 0, "repeated remove" },
		{ 2, 1, 1, 1, "toggle on using second atom" },
		{ 2, 1, 0, 0, "toggle off" },
		{ 1, 1, 1, 1, "add using second atom" },
		{ 0, 1, 1, 0, "remove using second atom" },
	};
	Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
	Atom fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	Atom unrelated[2] = {
		XInternAtom(dpy, "_NET_WM_STATE_STICKY", False),
		XInternAtom(dpy, "_NET_WM_STATE_DEMANDS_ATTENTION", False)
	};
	Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	Atom dialog = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	Atom atoms[3];
	Window barrier, win, pair[2], focused;
	XWindowAttributes normal = {0};
	unsigned int seeded;
	int floating, initial, revert;
	char kind[64];
	size_t i;

	XWarpPointer(dpy, None, root, 0, 0, 0, 0, 1, 1);
	for (floating = 0; floating < 2; floating++)
	for (initial = 0; initial < 2; initial++)
	for (seeded = 0; seeded <= 2; seeded++) {
		snprintf(kind, sizeof kind, "%s%s with %u other states", floating ? "floating" : "tiled",
		         initial ? " initial fullscreen" : "", seeded);
		barrier = XCreateSimpleWindow(dpy, root, 40, 40, 200, 150, 0, 0, 0);
		XSelectInput(dpy, barrier, StructureNotifyMask);
		XMapWindow(dpy, barrier);
		XFlush(dpy);
		CHECK(wait_clients(dpy, root, &barrier, 1, barrier), "state preservation barrier was not managed");
		win = XCreateSimpleWindow(dpy, root, 80, 90, 300, 200, 0, 0, 0);
		XStoreName(dpy, win, "fullscreen-state-preservation");
		if (floating)
			XChangeProperty(dpy, win, type, XA_ATOM, 32, PropModeReplace,
			                (unsigned char *)&dialog, 1);
		memcpy(atoms, unrelated, sizeof unrelated);
		atoms[seeded] = fullscreen;
		XChangeProperty(dpy, win, state, XA_ATOM, 32, PropModeReplace,
		                (unsigned char *)atoms, seeded + initial);
		XMapWindow(dpy, win);
		XFlush(dpy);
		pair[0] = barrier; pair[1] = win;
		CHECK(wait_clients(dpy, root, pair, 2, win), "%s: target was not managed", kind);
		focus_roundtrip(dpy, barrier, kind);
		check_state_atoms(dpy, win, atoms, seeded + initial, kind, "on mapping");
		if (initial) {
			fullscreen_request(dpy, root, win, barrier, 0, fullscreen, None);
			check_state_atoms(dpy, win, atoms, seeded, kind, "remove initial fullscreen");
		}
		CHECK(XGetWindowAttributes(dpy, win, &normal), "%s: cannot inspect normal geometry", kind);
		for (i = 0; i < sizeof steps / sizeof steps[0]; i++) {
			fullscreen_request(dpy, root, win, barrier, steps[i].action,
			                   steps[i].target && !steps[i].second ? fullscreen : unrelated[0],
			                   steps[i].second ? fullscreen : None);
			check_state_atoms(dpy, win, atoms, seeded + steps[i].full, kind, steps[i].step);
			check_fullscreen_state(dpy, win, &normal, steps[i].full, kind, steps[i].step);
			XGetInputFocus(dpy, &focused, &revert);
			CHECK(focused == win, "%s %s: state request changed focus", kind, steps[i].step);
		}
		XDestroyWindow(dpy, win);
		XDestroyWindow(dpy, barrier);
		XFlush(dpy);
		CHECK(wait_clients(dpy, root, NULL, 0, root), "state preservation fixture did not clean up its clients");
	}
}

/* Arrange through the configured Super+Shift+z binding after the application's
 * resize. It reselects the default layout, so dwm's layout slots stay unchanged.
 * The tiled client provides a reply even if the floating size did not change. */
static void
check_hint_resize(Display *dpy, Window root, Window win, Window barrier,
                  int width, int height, int expected_width, int expected_height,
                  const char *step)
{
	static int marker = 1;
	XEvent ev = {0};
	XWindowAttributes wa = {0};
	Window focused;
	int revert, i, replied = 0;

	while (XCheckTypedWindowEvent(dpy, barrier, ConfigureNotify, &ev))
		;
	XResizeWindow(dpy, win, width, height);
	memset(&ev, 0, sizeof ev);
	ev.xkey.type = KeyPress;
	ev.xkey.display = dpy;
	ev.xkey.window = ev.xkey.root = root;
	ev.xkey.same_screen = True;
	ev.xkey.state = Mod4Mask | ShiftMask;
	ev.xkey.keycode = XKeysymToKeycode(dpy, XK_z);
	/* dwm grabs keys rather than selecting KeyPressMask on root. */
	CHECK(ev.xkey.keycode && XSendEvent(dpy, root, False, SubstructureRedirectMask, &ev),
	      "%s: could not request a layout pass", step);
	/* A distinct border width rejects stale replies and replies from arrange(). */
	XSetWindowBorderWidth(dpy, barrier, ++marker);
	XResizeWindow(dpy, barrier, 17, 19);
	XFlush(dpy);
	for (i = 0; i < 100 && !replied; i++) {
		while (XCheckTypedWindowEvent(dpy, barrier, ConfigureNotify, &ev))
			if (ev.xconfigure.send_event && ev.xconfigure.border_width == marker)
				replied = 1;
		if (!replied)
			pause_millis(25);
	}
	CHECK(replied, "%s: dwm did not complete the size-hints round trip", step);
	CHECK(XGetWindowAttributes(dpy, win, &wa)
	      && wa.width == expected_width && wa.height == expected_height,
	      "%s: floating size is %dx%d, expected %dx%d", step,
	      wa.width, wa.height, expected_width, expected_height);
	XGetInputFocus(dpy, &focused, &revert);
	CHECK(focused == win, "%s: layout pass changed client focus", step);
}

static void
check_size_hints(Display *dpy, Window root)
{
	Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	Atom dialog = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	Window barrier, win, pair[2];
	XSizeHints hints = {0};

	/* Keep shrinking windows from generating pointer-driven focus changes. */
	XWarpPointer(dpy, None, root, 0, 0, 0, 0, 1, 1);
	XSync(dpy, False);
	barrier = XCreateSimpleWindow(dpy, root, 40, 40, 200, 150, 0, 0, 0);
	XSelectInput(dpy, barrier, StructureNotifyMask);
	XMapWindow(dpy, barrier);
	XFlush(dpy);
	CHECK(wait_clients(dpy, root, &barrier, 1, barrier), "size-hints barrier was not managed");
	win = XCreateSimpleWindow(dpy, root, 80, 90, 300, 200, 0, 0, 0);
	XStoreName(dpy, win, "live-size-hints");
	XChangeProperty(dpy, win, type, XA_ATOM, 32, PropModeReplace,
	                (unsigned char *)&dialog, 1);
	XMapWindow(dpy, win);
	XFlush(dpy);
	pair[0] = barrier; pair[1] = win;
	CHECK(wait_clients(dpy, root, pair, 2, win), "size-hints floating client was not managed");
	check_hint_resize(dpy, root, win, barrier, 137, 119, 137, 119, "absent hints");

	hints.flags = PMinSize | PMaxSize;
	hints.min_width = 80; hints.min_height = 60;
	hints.max_width = 140; hints.max_height = 100;
	XSetWMNormalHints(dpy, win, &hints);
	check_hint_resize(dpy, root, win, barrier, 40, 45, 80, 60, "live minimum");
	check_hint_resize(dpy, root, win, barrier, 200, 160, 140, 100, "live maximum");
	check_hint_resize(dpy, root, win, barrier, 80, 60, 80, 60, "exact minimum");
	check_hint_resize(dpy, root, win, barrier, 140, 100, 140, 100, "exact maximum");

	hints.flags = PBaseSize | PMinSize | PResizeInc;
	hints.base_width = 10; hints.base_height = 20;
	hints.min_width = hints.min_height = 50;
	hints.width_inc = 10; hints.height_inc = 15;
	XSetWMNormalHints(dpy, win, &hints);
	check_hint_resize(dpy, root, win, barrier, 137, 119, 130, 110, "base plus increments");
	hints.flags = PMinSize | PResizeInc;
	hints.min_width = 80; hints.min_height = 60;
	XSetWMNormalHints(dpy, win, &hints);
	check_hint_resize(dpy, root, win, barrier, 137, 119, 130, 105, "minimum used as base");

	hints.flags = PAspect;
	hints.min_aspect.x = hints.min_aspect.y = 1;
	hints.max_aspect.x = 2; hints.max_aspect.y = 1;
	XSetWMNormalHints(dpy, win, &hints);
	check_hint_resize(dpy, root, win, barrier, 300, 100, 200, 100, "maximum aspect");
	check_hint_resize(dpy, root, win, barrier, 100, 300, 100, 100, "minimum aspect");

	hints.flags = PSize;
	XSetWMNormalHints(dpy, win, &hints);
	check_hint_resize(dpy, root, win, barrier, 217, 173, 217, 173, "replacement clears old hints");
	hints.flags = PMinSize | PMaxSize;
	XSetWMNormalHints(dpy, win, &hints);
	check_hint_resize(dpy, root, win, barrier, 200, 160, 140, 100, "reinstalled maximum");
	XDeleteProperty(dpy, win, XA_WM_NORMAL_HINTS);
	check_hint_resize(dpy, root, win, barrier, 217, 173, 217, 173, "deletion clears old hints");
	{
		XSizeHints deleted;
		long supplied;

		CHECK(!XGetWMNormalHints(dpy, win, &deleted, &supplied),
		      "deleted WM_NORMAL_HINTS is still present on the X server");
	}
	hints.flags = PSize;
	XSetWMNormalHints(dpy, win, &hints);
	check_hint_resize(dpy, root, win, barrier, 217, 173, 217, 173, "replacement after deletion");

	XDestroyWindow(dpy, win);
	XDestroyWindow(dpy, barrier);
	XFlush(dpy);
	CHECK(wait_clients(dpy, root, NULL, 0, root), "size-hints fixture did not clean up its clients");
}

/* _NET_WM_STATE is an unordered ATOM list, not just its first element. */
static void
check_fullscreen_properties(Display *dpy, Window root)
{
	static const struct {
		const char *name;
		int count, format, cardinal, full;
		int atoms[10]; /* 1 is fullscreen; 0 is an unrelated state. */
	} cases[] = {
		{ "absent property", -1, 32, 0, 0, {0} },
		{ "empty ATOM list", 0, 32, 0, 0, {0} },
		{ "unrelated states only", 2, 32, 0, 0, {0, 0} },
		{ "fullscreen alone", 1, 32, 0, 1, {1} },
		{ "fullscreen first", 2, 32, 0, 1, {1, 0} },
		{ "fullscreen last", 2, 32, 0, 1, {0, 1} },
		{ "fullscreen middle", 3, 32, 0, 1, {0, 1, 0} },
		{ "fullscreen beyond initial read", 10, 32, 0, 1, {0, 0, 0, 0, 0, 0, 0, 0, 0, 1} },
		{ "duplicate fullscreen", 2, 32, 0, 1, {1, 1} },
		{ "wrong CARDINAL type", 1, 32, 1, 0, {1} },
		{ "wrong 8-bit format", 1, 8, 0, 0, {1} },
		{ "wrong 16-bit format", 1, 16, 0, 0, {1} },
	};
	Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
	Atom fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	Atom unrelated = XInternAtom(dpy, "_DWM_TEST_UNRELATED_STATE", False);
	Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	Atom dialog = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	Atom atoms[10];
	unsigned char byte = (unsigned char)fullscreen;
	unsigned short word = (unsigned short)fullscreen;
	const unsigned char *data;
	Window barrier, win, pair[2], focused;
	XWindowAttributes wa = {0}, resized = {0};
	int floating, j, revert, full;
	size_t i;
	char step[128];

	XWarpPointer(dpy, None, root, 0, 0, 0, 0, 1, 1);
	XSync(dpy, False);
	barrier = XCreateSimpleWindow(dpy, root, 40, 40, 200, 150, 0, 0, 0);
	XSelectInput(dpy, barrier, StructureNotifyMask);
	XMapWindow(dpy, barrier);
	XFlush(dpy);
	CHECK(wait_clients(dpy, root, &barrier, 1, barrier), "fullscreen-properties barrier was not managed");
	for (floating = 0; floating < 2; floating++) {
		for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
			snprintf(step, sizeof step, "%s %s", floating ? "floating" : "tiled", cases[i].name);
			win = XCreateSimpleWindow(dpy, root, 80, 90, 300, 200, 0, 0, 0);
			XStoreName(dpy, win, step);
			if (floating)
				XChangeProperty(dpy, win, type, XA_ATOM, 32, PropModeReplace,
				                (unsigned char *)&dialog, 1);
			for (j = 0; j < cases[i].count; j++)
				atoms[j] = cases[i].atoms[j] ? fullscreen : unrelated;
			data = cases[i].format == 8 ? &byte : cases[i].format == 16
			     ? (unsigned char *)&word : (unsigned char *)atoms;
			if (cases[i].count >= 0)
				XChangeProperty(dpy, win, state, cases[i].cardinal ? XA_CARDINAL : XA_ATOM,
				                cases[i].format, PropModeReplace, data, cases[i].count);
			XMapWindow(dpy, win);
			XFlush(dpy);
			pair[0] = barrier; pair[1] = win;
			CHECK(wait_clients(dpy, root, pair, 2, win), "%s: target was not managed and focused", step);
			focus_roundtrip(dpy, barrier, step);
			CHECK(XGetWindowAttributes(dpy, win, &wa), "%s: cannot inspect mapped geometry", step);
			full = wa.x == 0 && wa.y == 0 && wa.border_width == 0
			    && wa.width == DisplayWidth(dpy, DefaultScreen(dpy))
			    && wa.height == DisplayHeight(dpy, DefaultScreen(dpy));
			CHECK(full == cases[i].full,
			      "%s: initial fullscreen geometry is %d, expected %d (%d,%d %dx%d border %d)",
			      step, full, cases[i].full, wa.x, wa.y, wa.width, wa.height, wa.border_width);
			if (!cases[i].full && floating)
				CHECK(wa.width == 300 && wa.height == 200 && wa.border_width > 0,
				      "%s: normal dialog lost its requested size or border", step);
			XResizeWindow(dpy, win, 137, 119);
			focus_roundtrip(dpy, barrier, step);
			CHECK(XGetWindowAttributes(dpy, win, &resized), "%s: cannot inspect resized geometry", step);
			if (!cases[i].full && floating)
				CHECK(resized.width == 137 && resized.height == 119,
				      "%s: normal dialog did not accept its resize", step);
			else
				CHECK(resized.x == wa.x && resized.y == wa.y && resized.width == wa.width
				      && resized.height == wa.height && resized.border_width == wa.border_width,
				      "%s: %s client accepted a resize (%d,%d %dx%d border %d -> %d,%d %dx%d border %d)",
				      step, cases[i].full ? "fullscreen" : "tiled",
				      wa.x, wa.y, wa.width, wa.height, wa.border_width,
				      resized.x, resized.y, resized.width, resized.height, resized.border_width);
			XGetInputFocus(dpy, &focused, &revert);
			CHECK(focused == win, "%s: resize changed focus", step);
			XDestroyWindow(dpy, win);
			XFlush(dpy);
			CHECK(wait_clients(dpy, root, &barrier, 1, barrier), "%s: target was not removed", step);
		}
	}
	XDestroyWindow(dpy, barrier);
	XFlush(dpy);
	CHECK(wait_clients(dpy, root, NULL, 0, root), "fullscreen-properties fixture did not clean up its clients");
}

static void
focus_next(Display *dpy, Window root, Window barrier, const char *step)
{
	XEvent ev = {0};

	ev.xkey.type = KeyPress;
	ev.xkey.display = dpy;
	ev.xkey.window = ev.xkey.root = root;
	ev.xkey.same_screen = True;
	ev.xkey.state = Mod4Mask;
	ev.xkey.keycode = XKeysymToKeycode(dpy, XK_j);
	CHECK(ev.xkey.keycode && XSendEvent(dpy, root, False, SubstructureRedirectMask, &ev),
	      "%s: could not send Super+j", step);
	focus_roundtrip(dpy, barrier, step);
}

static void
check_focus_result(Display *dpy, Window root, Window win, Window expected,
                   int takefocus, const char *step)
{
	Atom protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
	Atom take = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
	Atom active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False), type;
	XEvent ev;
	Window focused;
	unsigned char *data = NULL;
	unsigned long n, after;
	int revert, format, received = 0;

	XGetInputFocus(dpy, &focused, &revert);
	CHECK(focused == expected, "%s: keyboard focus is 0x%lx, expected 0x%lx",
	      step, focused, expected);
	while (XCheckTypedWindowEvent(dpy, win, ClientMessage, &ev)) {
		CHECK(ev.xclient.message_type == protocols && ev.xclient.format == 32
		      && (Atom)ev.xclient.data.l[0] == take,
		      "%s: unexpected focus protocol message", step);
		if (ev.xclient.message_type == protocols && ev.xclient.format == 32
		&& (Atom)ev.xclient.data.l[0] == take)
			received++;
	}
	CHECK((received > 0) == takefocus, "%s: received %d WM_TAKE_FOCUS messages, expected %s",
	      step, received, takefocus ? "at least one" : "none");
	if (expected == win) {
		CHECK(XGetWindowProperty(dpy, root, active, 0, 1, False, XA_WINDOW,
		                         &type, &format, &n, &after, &data) == Success
		      && type == XA_WINDOW && format == 32 && n == 1 && !after
		      && ((Window *)data)[0] == win,
		      "%s: directly focused client is not _NET_ACTIVE_WINDOW", step);
		if (data)
			XFree(data);
	}
}

/* dwm's activation policy marks background clients urgent instead of stealing focus. */
static void
check_active_window_messages(Display *dpy, Window root)
{
	static const int formats[] = { 8, 16, 32 };
	static const struct { const char *name; int selected, urgent; } cases[] = {
		{ "selected", 1, 0 },
		{ "background", 0, 0 },
		{ "already urgent background", 0, 1 },
	};
	Atom active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	Window barrier, win, pair[2], expected_focus;
	XWMHints hints = {0}, *actual;
	XWindowAttributes before = {0}, after = {0};
	XEvent ev;
	char step[128];
	int source, repeat, expected_urgent;
	size_t i, j;

	XWarpPointer(dpy, None, root, 0, 0, 0, 0, 1100, 5);
	barrier = XCreateSimpleWindow(dpy, root, 40, 40, 200, 150, 0, 0, 0);
	XSelectInput(dpy, barrier, StructureNotifyMask);
	XMapWindow(dpy, barrier);
	XFlush(dpy);
	CHECK(wait_clients(dpy, root, &barrier, 1, barrier), "activation barrier was not managed");
	for (j = 0; j < sizeof formats / sizeof formats[0]; j++)
		for (source = 0; source <= 2; source++)
			for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
				/* Fresh clients isolate cached urgency as well as the property. */
				win = XCreateSimpleWindow(dpy, root, 80, 90, 300, 200, 0, 0, 0);
				XStoreName(dpy, win, "active-window-messages");
				hints.flags = InputHint | StateHint;
				hints.input = True;
				hints.initial_state = NormalState;
				XSetWMHints(dpy, win, &hints);
				XMapWindow(dpy, win);
				XFlush(dpy);
				pair[0] = barrier; pair[1] = win;
				CHECK(wait_clients(dpy, root, pair, 2, win), "activation target was not managed");
				if (!cases[i].selected)
					focus_next(dpy, root, barrier, "activation background setup");
				expected_focus = cases[i].selected ? win : barrier;
				hints.flags |= cases[i].urgent ? XUrgencyHint : 0;
				XSetWMHints(dpy, win, &hints);
				focus_roundtrip(dpy, barrier, "activation hint setup");
				actual = XGetWMHints(dpy, win);
				CHECK(actual && actual->flags == hints.flags && actual->input == True
				      && actual->initial_state == NormalState, "activation hint reset failed");
				if (actual)
					XFree(actual);
				check_focus_result(dpy, root, expected_focus, expected_focus, 0, "activation focus reset");
				CHECK(XGetWindowAttributes(dpy, win, &before), "cannot inspect activation geometry");
				expected_urgent = cases[i].urgent || (formats[j] == 32 && !cases[i].selected);
				for (repeat = 0; repeat < 2; repeat++) {
					snprintf(step, sizeof step, "format %d source %d %s request %d",
					         formats[j], source, cases[i].name, repeat + 1);
					memset(&ev, 0, sizeof ev);
					ev.xclient.type = ClientMessage;
					ev.xclient.window = win;
					ev.xclient.message_type = active;
					ev.xclient.format = formats[j];
					if (formats[j] == 32) {
						ev.xclient.data.l[0] = source;
						ev.xclient.data.l[1] = CurrentTime;
						ev.xclient.data.l[2] = expected_focus;
					} else {
						/* Ordinary malformed bytes, not a native union-shaped payload. */
						memset(ev.xclient.data.b, 0xa5, sizeof ev.xclient.data.b);
						if (formats[j] == 8)
							ev.xclient.data.b[0] = source;
						else
							ev.xclient.data.s[0] = source;
					}
					CHECK(XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev),
					      "%s: could not send activation request", step);
					focus_roundtrip(dpy, barrier, step);
					actual = XGetWMHints(dpy, win);
					CHECK(actual && !!(actual->flags & XUrgencyHint) == expected_urgent,
					      "%s: urgency is %d, expected %d", step,
					      actual ? !!(actual->flags & XUrgencyHint) : -1, expected_urgent);
					CHECK(actual && (actual->flags & ~XUrgencyHint) == (InputHint | StateHint)
					      && actual->input == True && actual->initial_state == NormalState,
					      "%s: activation damaged unrelated WM_HINTS fields", step);
					if (actual)
						XFree(actual);
					check_focus_result(dpy, root, expected_focus, expected_focus, 0, step);
					CHECK(XGetWindowAttributes(dpy, win, &after)
					      && after.x == before.x && after.y == before.y
					      && after.width == before.width && after.height == before.height
					      && after.border_width == before.border_width,
					      "%s: activation changed geometry or borders", step);
				}
				if (!cases[i].selected) {
					focus_next(dpy, root, barrier, "activation user focus clears urgency");
					check_focus_result(dpy, root, win, win, 0, "activation user focus");
					actual = XGetWMHints(dpy, win);
					CHECK(actual && !(actual->flags & XUrgencyHint), "user focus did not clear urgency");
					if (actual)
						XFree(actual);
				}
				XDestroyWindow(dpy, win);
				XFlush(dpy);
				CHECK(wait_clients(dpy, root, &barrier, 1, barrier), "activation target did not clean up");
			}
	XDestroyWindow(dpy, barrier);
	XFlush(dpy);
	CHECK(wait_clients(dpy, root, NULL, 0, root), "activation fixture did not clean up its clients");
}

static void
check_focus_cycle(Display *dpy, Window root, Window win, Window barrier,
                  int direct, int takefocus, const char *step)
{
	XEvent ev;
	Window focused;
	int revert;

	focus_next(dpy, root, barrier, step);
	XGetInputFocus(dpy, &focused, &revert);
	CHECK(focused == barrier, "%s: could not focus the other client", step);
	while (XCheckTypedWindowEvent(dpy, win, ClientMessage, &ev))
		;
	focus_next(dpy, root, barrier, step);
	check_focus_result(dpy, root, win, direct ? win : barrier, takefocus, step);
}

static void
check_focus_hints(Display *dpy, Window root)
{
	static const struct {
		const char *name;
		int input, takefocus;
	} cases[] = {
		{ "absent hints", -1, 0 }, { "absent hints with protocol", -1, 1 },
		{ "missing InputHint", -2, 0 }, { "missing InputHint with protocol", -2, 1 },
		{ "InputHint true", 1, 0 }, { "InputHint true with protocol", 1, 1 },
		{ "InputHint false", 0, 0 }, { "InputHint false with protocol", 0, 1 },
	};
	Atom take = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
	Atom protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
	Window barrier, win, pair[2], child;
	XWMHints hints = {0}, *deleted;
	unsigned int i;

	XWarpPointer(dpy, None, root, 0, 0, 0, 0, 1, 1);
	XSync(dpy, False);
	barrier = XCreateSimpleWindow(dpy, root, 40, 40, 200, 150, 0, 0, 0);
	XSelectInput(dpy, barrier, StructureNotifyMask);
	XMapWindow(dpy, barrier);
	XFlush(dpy);
	CHECK(wait_clients(dpy, root, &barrier, 1, barrier), "focus barrier was not managed");
	for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		win = XCreateSimpleWindow(dpy, root, 80, 90, 300, 200, 0, 0, 0);
		XStoreName(dpy, win, cases[i].name);
		if (cases[i].input != -1) {
			hints.flags = cases[i].input == -2 ? StateHint : InputHint;
			hints.input = cases[i].input == 1;
			hints.initial_state = NormalState;
			XSetWMHints(dpy, win, &hints);
		}
		if (cases[i].takefocus)
			XSetWMProtocols(dpy, win, &take, 1);
		XMapWindow(dpy, win);
		XFlush(dpy);
		pair[0] = barrier; pair[1] = win;
		CHECK(wait_clients(dpy, root, pair, 2, cases[i].input ? win : barrier),
		      "%s: mapped client list or focus is incorrect", cases[i].name);
		focus_roundtrip(dpy, barrier, cases[i].name);
		check_focus_result(dpy, root, win, cases[i].input ? win : barrier,
		                   cases[i].takefocus, cases[i].name);
		check_focus_cycle(dpy, root, win, barrier, cases[i].input != 0,
		                  cases[i].takefocus, cases[i].name);
		if (cases[i].input == 0 && cases[i].takefocus) {
			/* A globally active client handles WM_TAKE_FOCUS by focusing its child. */
			child = XCreateSimpleWindow(dpy, win, 0, 0, 50, 50, 0, 0, 0);
			XMapWindow(dpy, child);
			XSetInputFocus(dpy, child, RevertToParent, CurrentTime);
			focus_roundtrip(dpy, barrier, "application focuses its child");
			check_focus_result(dpy, root, win, child, 0, "application focuses its child");
		}
		/* Change both hints and protocols while the client remains managed. */
		hints.flags = InputHint;
		hints.input = False;
		XSetWMHints(dpy, win, &hints);
		XSetWMProtocols(dpy, win, &take, 1);
		check_focus_cycle(dpy, root, win, barrier, 0, 1, "live InputHint false and protocol add");
		hints.input = True;
		XSetWMHints(dpy, win, &hints);
		XDeleteProperty(dpy, win, protocols);
		check_focus_cycle(dpy, root, win, barrier, 1, 0, "live InputHint true and protocol deletion");
		hints.input = False;
		XSetWMHints(dpy, win, &hints);
		check_focus_cycle(dpy, root, win, barrier, 0, 0, "live InputHint false without protocol");
		hints.flags = StateHint;
		XSetWMHints(dpy, win, &hints);
		check_focus_cycle(dpy, root, win, barrier, 1, 0, "replacement omits InputHint");
		hints.flags = InputHint;
		XSetWMHints(dpy, win, &hints);
		check_focus_cycle(dpy, root, win, barrier, 0, 0, "reinstalled InputHint false");
		XDeleteProperty(dpy, win, XA_WM_HINTS);
		check_focus_cycle(dpy, root, win, barrier, 1, 0, "WM_HINTS deletion restores direct focus");
		deleted = XGetWMHints(dpy, win);
		CHECK(!deleted, "deleted WM_HINTS is still present on the X server");
		if (deleted)
			XFree(deleted);
		hints.flags = StateHint;
		XSetWMHints(dpy, win, &hints);
		check_focus_cycle(dpy, root, win, barrier, 1, 0, "replacement after WM_HINTS deletion");
		XDestroyWindow(dpy, win);
		XFlush(dpy);
		CHECK(wait_clients(dpy, root, &barrier, 1, barrier), "focus target did not clean up");
	}
	XDestroyWindow(dpy, barrier);
	XFlush(dpy);
	CHECK(wait_clients(dpy, root, NULL, 0, root), "focus fixture did not clean up its clients");
}

static void
check_unmanage(Display *dpy, Window root)
{
	Window a, b, pair[2];
	XWindowAttributes split = {0}, expanded = {0};

	a = XCreateSimpleWindow(dpy, root, 0, 0, 200, 200, 0, 0, 0);
	b = XCreateSimpleWindow(dpy, root, 0, 0, 200, 200, 0, 0, 0);
	XMapWindow(dpy, a);
	XFlush(dpy);
	CHECK(wait_clients(dpy, root, &a, 1, a), "first client was not managed and focused");
	XMapWindow(dpy, b);
	XFlush(dpy);
	pair[0] = a; pair[1] = b;
	CHECK(wait_clients(dpy, root, pair, 2, b), "second client was not managed and focused");
	CHECK(XGetWindowAttributes(dpy, a, &split), "could not inspect the split layout");

	XUnmapWindow(dpy, b);
	XFlush(dpy);
	CHECK(wait_clients(dpy, root, &a, 1, a), "unmapping did not remove the client and focus its neighbor");
	CHECK(XGetWindowAttributes(dpy, a, &expanded) && expanded.width > split.width,
	      "unmapping did not rearrange the remaining tiled client");

	XMapWindow(dpy, b);
	XFlush(dpy);
	CHECK(wait_clients(dpy, root, pair, 2, b), "unmapped client was not managed again");
	XDestroyWindow(dpy, b);
	XFlush(dpy);
	CHECK(wait_clients(dpy, root, &a, 1, a), "destroying did not remove the client and focus its neighbor");
	CHECK(XGetWindowAttributes(dpy, a, &expanded) && expanded.width > split.width,
	      "destroying did not rearrange the remaining tiled client");
	XDestroyWindow(dpy, a);
	XFlush(dpy);
	CHECK(wait_clients(dpy, root, NULL, 0, root), "last client removal did not clear the list and focus");
}

/* Only capture the left half: sampler/status updates on the right are unrelated. */
static XImage *
title_image(Display *dpy, Window root, Window barrier, const char *step)
{
	XWindowAttributes wa;
	Window bar;
	XImage *image = NULL;

	focus_roundtrip(dpy, barrier, step);
	bar = find_bar(dpy, root, &wa);
	if (bar != None)
		image = XGetImage(dpy, bar, 0, 0, wa.width / 2, wa.height, AllPlanes, ZPixmap);
	CHECK(image != NULL, "%s: could not capture the title bar", step);
	return image;
}

static int
image_difference(XImage *a, XImage *b)
{
	int x, y, different = 0;

	if (!a || !b || a->width != b->width || a->height != b->height)
		return -1;
	for (y = 0; y < a->height; y++)
		for (x = 0; x < a->width; x++)
			different += XGetPixel(a, x, y) != XGetPixel(b, x, y);
	return different;
}

static void
check_title_image(Display *dpy, Window root, Window win, Window barrier,
                  XImage *expected, const char *step)
{
	XImage *actual = title_image(dpy, root, barrier, step);
	Window focused;
	int revert, different = image_difference(actual, expected);

	CHECK(different == 0, "%s: title bar differs from the expected title by %d pixels",
	      step, different);
	XGetInputFocus(dpy, &focused, &revert);
	CHECK(focused == win, "%s: title update changed keyboard focus", step);
	if (actual)
		XDestroyImage(actual);
}

static void
check_title_absent(Display *dpy, Window win, Atom atom, const char *step)
{
	Atom type;
	int format;
	unsigned long count, after;
	unsigned char *data = NULL;

	CHECK(XGetWindowProperty(dpy, win, atom, 0, 0, False, AnyPropertyType,
	                         &type, &format, &count, &after, &data) == Success
	      && type == None, "%s: deleted title property is still present", step);
	if (data)
		XFree(data);
}

/* Compare the same focused client so only the urgency rendering can change. */
static void
check_urgency_image(Display *dpy, Window root, Window barrier, XImage *expected,
                    const char *step)
{
	XImage *actual = title_image(dpy, root, barrier, step);
	int different = image_difference(actual, expected);

	CHECK(different == 0, "%s: urgency bar differs from nonurgent reference by %d pixels",
	      step, different);
	check_focus_result(dpy, root, barrier, barrier, 0, step);
	if (actual)
		XDestroyImage(actual);
}

static void
check_urgency_lifecycle(Display *dpy, Window root)
{
	Atom active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	Window barrier, win, pair[2];
	XWMHints hints = {0}, *actual;
	XImage *normal, *urgent;
	XEvent ev;
	char step[128];
	int activation;

	XWarpPointer(dpy, None, root, 0, 0, 0, 0, 1, 1);
	barrier = XCreateSimpleWindow(dpy, root, 40, 40, 200, 150, 0, 0, 0);
	XSelectInput(dpy, barrier, StructureNotifyMask);
	XMapWindow(dpy, barrier);
	XFlush(dpy);
	CHECK(wait_clients(dpy, root, &barrier, 1, barrier), "urgency barrier was not managed");
	for (activation = 0; activation <= 1; activation++) {
		win = XCreateSimpleWindow(dpy, root, 80, 80, 200, 150, 0, 0, 0);
		XStoreName(dpy, win, "urgency-lifecycle");
		hints.flags = InputHint | StateHint;
		hints.input = True;
		hints.initial_state = NormalState;
		XSetWMHints(dpy, win, &hints);
		XMapWindow(dpy, win);
		XFlush(dpy);
		pair[0] = barrier; pair[1] = win;
		CHECK(wait_clients(dpy, root, pair, 2, win), "urgency target was not managed");
		focus_next(dpy, root, barrier, "urgency background setup");
		check_focus_result(dpy, root, barrier, barrier, 0, "urgency background setup");
		normal = title_image(dpy, root, barrier, "nonurgent reference");
		/* Deleting an already nonurgent property is a control. */
		XDeleteProperty(dpy, win, XA_WM_HINTS);
		check_urgency_image(dpy, root, barrier, normal, "nonurgent WM_HINTS deletion");
		check_title_absent(dpy, win, XA_WM_HINTS, "nonurgent WM_HINTS deletion");
		XSetWMHints(dpy, win, &hints);
		focus_roundtrip(dpy, barrier, "urgency hint recovery");
		if (activation) {
			memset(&ev, 0, sizeof ev);
			ev.xclient.type = ClientMessage;
			ev.xclient.window = win;
			ev.xclient.message_type = active;
			ev.xclient.format = 32;
			ev.xclient.data.l[0] = 1;
			ev.xclient.data.l[1] = CurrentTime;
			ev.xclient.data.l[2] = barrier;
			CHECK(XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev),
			      "could not send valid urgency activation request");
		} else {
			hints.flags |= XUrgencyHint;
			XSetWMHints(dpy, win, &hints);
		}
		urgent = title_image(dpy, root, barrier, "urgent reference");
		CHECK(image_difference(normal, urgent) > 0, "urgency did not visibly change the bar");
		actual = XGetWMHints(dpy, win);
		CHECK(actual && (actual->flags & XUrgencyHint), "urgent reference lacks XUrgencyHint");
		if (actual)
			XFree(actual);
		check_focus_result(dpy, root, barrier, barrier, 0, "urgent reference");

		XDeleteProperty(dpy, win, XA_WM_HINTS);
		snprintf(step, sizeof step, "%s urgency: WM_HINTS deletion",
		         activation ? "activation-set" : "application-set");
		check_urgency_image(dpy, root, barrier, normal, step);
		check_title_absent(dpy, win, XA_WM_HINTS, step);
		/* A root status replacement forces drawbar without touching client hints. */
		XStoreName(dpy, root, "urgency-redraw");
		snprintf(step, sizeof step, "%s urgency: deletion after forced redraw",
		         activation ? "activation-set" : "application-set");
		check_urgency_image(dpy, root, barrier, normal, step);

		/* Replacement must recover the cache; focus must clear newly set urgency. */
		hints.flags &= ~XUrgencyHint;
		XSetWMHints(dpy, win, &hints);
		check_urgency_image(dpy, root, barrier, normal, "nonurgent replacement recovery");
		hints.flags |= XUrgencyHint;
		XSetWMHints(dpy, win, &hints);
		focus_roundtrip(dpy, barrier, "urgency before user focus");
		focus_next(dpy, root, barrier, "user focus clears urgency");
		check_focus_result(dpy, root, win, win, 0, "user focus clears urgency");
		actual = XGetWMHints(dpy, win);
		CHECK(actual && !(actual->flags & XUrgencyHint), "user focus did not clear XUrgencyHint");
		if (actual)
			XFree(actual);
		focus_next(dpy, root, barrier, "return to background after urgency clear");
		check_urgency_image(dpy, root, barrier, normal, "user focus clears urgency rendering");
		XDestroyWindow(dpy, win);
		XFlush(dpy);
		CHECK(wait_clients(dpy, root, &barrier, 1, barrier), "urgency target did not clean up");
		if (normal)
			XDestroyImage(normal);
		if (urgent)
			XDestroyImage(urgent);
	}
	XDeleteProperty(dpy, root, XA_WM_NAME);
	focus_roundtrip(dpy, barrier, "urgency status cleanup");
	XDestroyWindow(dpy, barrier);
	XFlush(dpy);
	CHECK(wait_clients(dpy, root, NULL, 0, root), "urgency fixture did not clean up its clients");
}

static void
check_title_updates(Display *dpy, Window root)
{
	const char *names[] = { "legacy-name", "caf\303\251-title", "updated-title" };
	const char *steps[] = { "legacy title", "UTF-8 title reference", "updated title reference" };
	Atom netname = XInternAtom(dpy, "_NET_WM_NAME", False);
	Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	Window barrier, win, pair[2];
	XImage *missing, *reference[3];
	int i, j;

	XWarpPointer(dpy, None, root, 0, 0, 0, 0, 1, 1);
	barrier = XCreateSimpleWindow(dpy, root, 40, 40, 200, 150, 0, 0, 0);
	XSelectInput(dpy, barrier, StructureNotifyMask);
	XMapWindow(dpy, barrier);
	XFlush(dpy);
	CHECK(wait_clients(dpy, root, &barrier, 1, barrier), "title barrier was not managed");
	win = XCreateSimpleWindow(dpy, root, 80, 80, 200, 150, 0, 0, 0);
	XMapWindow(dpy, win);
	XFlush(dpy);
	pair[0] = barrier; pair[1] = win;
	CHECK(wait_clients(dpy, root, pair, 2, win), "title target was not managed");
	missing = title_image(dpy, root, barrier, "initially absent titles");
	for (i = 0; i < 3; i++) {
		XStoreName(dpy, win, names[i]);
		reference[i] = title_image(dpy, root, barrier, steps[i]);
		CHECK(image_difference(reference[i], missing) > 0,
		      "%s: visible title did not differ from the unnamed client", steps[i]);
		for (j = 0; j < i; j++)
			CHECK(image_difference(reference[i], reference[j]) > 0,
			      "%s: distinct titles rendered identically", steps[i]);
	}

	XStoreName(dpy, win, names[0]);
	check_title_image(dpy, root, win, barrier, reference[0], "live WM_NAME replacement");
	XChangeProperty(dpy, win, netname, utf8, 8, PropModeReplace,
	                (const unsigned char *)names[1], strlen(names[1]));
	check_title_image(dpy, root, win, barrier, reference[1], "UTF-8 _NET_WM_NAME takes precedence");
	XStoreName(dpy, win, names[2]);
	check_title_image(dpy, root, win, barrier, reference[1], "lower-priority WM_NAME update");
	XDeleteProperty(dpy, win, XA_WM_NAME);
	check_title_image(dpy, root, win, barrier, reference[1], "lower-priority WM_NAME deletion");
	check_title_absent(dpy, win, XA_WM_NAME, "lower-priority WM_NAME deletion");
	XStoreName(dpy, win, names[2]);
	check_title_image(dpy, root, win, barrier, reference[1], "lower-priority WM_NAME reinstallation");
	XChangeProperty(dpy, win, netname, utf8, 8, PropModeReplace,
	                (const unsigned char *)names[0], strlen(names[0]));
	check_title_image(dpy, root, win, barrier, reference[0], "live _NET_WM_NAME replacement");
	XChangeProperty(dpy, win, netname, utf8, 8, PropModeReplace, NULL, 0);
	check_title_image(dpy, root, win, barrier, reference[2], "empty _NET_WM_NAME falls back to WM_NAME");

	XChangeProperty(dpy, win, netname, utf8, 8, PropModeReplace,
	                (const unsigned char *)names[1], strlen(names[1]));
	check_title_image(dpy, root, win, barrier, reference[1], "reinstalled UTF-8 title");
	XDeleteProperty(dpy, win, netname);
	check_title_image(dpy, root, win, barrier, reference[2], "_NET_WM_NAME deletion falls back to WM_NAME");
	check_title_absent(dpy, win, netname, "_NET_WM_NAME deletion");
	focus_next(dpy, root, barrier, "focus away after title deletion");
	focus_next(dpy, root, barrier, "focus back after title deletion");
	check_title_image(dpy, root, win, barrier, reference[2], "fallback survives focus redraw");

	/* A real replacement recovers the cache before testing the next deletion. */
	XStoreName(dpy, win, names[0]);
	check_title_image(dpy, root, win, barrier, reference[0], "WM_NAME replacement after deletion");
	XDeleteProperty(dpy, win, XA_WM_NAME);
	check_title_image(dpy, root, win, barrier, missing, "WM_NAME deletion restores unnamed title");
	check_title_absent(dpy, win, XA_WM_NAME, "WM_NAME deletion");
	XStoreName(dpy, win, "");
	check_title_image(dpy, root, win, barrier, missing, "empty WM_NAME restores unnamed title");

	XDestroyWindow(dpy, win);
	XFlush(dpy);
	CHECK(wait_clients(dpy, root, &barrier, 1, barrier), "title target did not clean up");
	/* Mapping must use the same precedence as live updates. */
	win = XCreateSimpleWindow(dpy, root, 80, 80, 200, 150, 0, 0, 0);
	XStoreName(dpy, win, names[0]);
	XChangeProperty(dpy, win, netname, utf8, 8, PropModeReplace,
	                (const unsigned char *)names[1], strlen(names[1]));
	XMapWindow(dpy, win);
	XFlush(dpy);
	pair[1] = win;
	CHECK(wait_clients(dpy, root, pair, 2, win), "pre-named title target was not managed");
	check_title_image(dpy, root, win, barrier, reference[1], "UTF-8 title precedence on mapping");
	XDestroyWindow(dpy, win);
	XDestroyWindow(dpy, barrier);
	XFlush(dpy);
	CHECK(wait_clients(dpy, root, NULL, 0, root), "title fixture did not clean up its clients");
	if (missing)
		XDestroyImage(missing);
	for (i = 0; i < 3; i++)
		if (reference[i])
			XDestroyImage(reference[i]);
}

int
main(int argc, char *argv[])
{
	Display *dpy;
	Window root, tiled, floating, transient, target;
	XWindowAttributes attributes = {0};
	XClassHint class_hint = { "integration", "Integration" };
	XConfigureEvent configure_event;
	int i, got_configure, floating_managed = 0;

	dpy = XOpenDisplay(NULL);
	CHECK(dpy != NULL, "could not open the private X display");
	if (!dpy)
		return EXIT_FAILURE;
	root = DefaultRootWindow(dpy);

	/* x11 bar-background <#rrggbb>: most of the bar shows that background */
	if (argc == 3 && !strcmp(argv[1], "bar-background")) {
		XWindowAttributes bar = {0};
		unsigned int area, count = 0;
		int i;

		CHECK(find_bar(dpy, root, &bar) != None, "dwm has no bar");
		area = (unsigned int)(bar.width * bar.height);
		for (i = 0; i < 80; i++) {
			count = count_bar_pixels(dpy, root, argv[2], 0, NULL);
			if (count > area / 2)
				break;
			pause_millis(25);
		}
		CHECK(count > area / 2, "the bar background is not %s (%u of %u pixels)",
		      argv[2], count, area);
		XCloseDisplay(dpy);
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	/* x11 bar-color <#rrggbb> <pixels>: the status left by a previous run shows that color */
	if (argc == 4 && !strcmp(argv[1], "bar-color")) {
		unsigned int want = (unsigned int)atoi(argv[3]);
		unsigned int count = wait_bar_color(dpy, root, argv[2],
		                                    DisplayWidth(dpy, DefaultScreen(dpy)) - 400, want, NULL);

		CHECK(count == want, "the bar shows %u pixels of %s (expected %u)", count, argv[2], want);
		XCloseDisplay(dpy);
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	/* x11 atom-format: _NET_WM_STATE and _NET_CLIENT_INFO are read only as 32-bit data */
	if (argc == 2 && !strcmp(argv[1], "atom-format")) {
		check_atom_format(dpy, root);
		check_client_info(dpy, root);
		XCloseDisplay(dpy);
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	/* x11 titles: rendered title precedence, live updates and property deletion */
	if (argc == 2 && !strcmp(argv[1], "titles")) {
		check_title_updates(dpy, root);
		XCloseDisplay(dpy);
		if (!failures)
			puts("X11 live title checks passed");
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	/* x11 focus-hints: ICCCM input models and live focus protocols */
	if (argc == 2 && !strcmp(argv[1], "focus-hints")) {
		check_focus_hints(dpy, root);
		XCloseDisplay(dpy);
		if (!failures)
			puts("X11 focus-hints checks passed");
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	if (argc == 2 && !strcmp(argv[1], "active-window-messages")) {
		check_active_window_messages(dpy, root);
		XCloseDisplay(dpy);
		if (!failures)
			puts("X11 active-window client-message checks passed");
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	if (argc == 2 && !strcmp(argv[1], "urgency-lifecycle")) {
		check_urgency_lifecycle(dpy, root);
		XCloseDisplay(dpy);
		if (!failures)
			puts("X11 urgency lifecycle checks passed");
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	/* x11 size-hints: live ICCCM constraints, replacements and deletion */
	if (argc == 2 && !strcmp(argv[1], "size-hints")) {
		check_size_hints(dpy, root);
		XCloseDisplay(dpy);
		if (!failures)
			puts("X11 live size-hints checks passed");
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	/* x11 fullscreen-properties: initial state lists and format/type guards */
	if (argc == 2 && !strcmp(argv[1], "fullscreen-properties")) {
		check_fullscreen_properties(dpy, root);
		XCloseDisplay(dpy);
		if (!failures)
			puts("X11 initial fullscreen-property checks passed");
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	if (argc == 2 && !strcmp(argv[1], "fullscreen-state-preservation")) {
		check_fullscreen_state_preservation(dpy, root);
		XCloseDisplay(dpy);
		if (!failures)
			puts("X11 fullscreen state-preservation checks passed");
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	if (argc == 2 && !strcmp(argv[1], "initial-fullscreen-exit")) {
		check_initial_fullscreen_exit(dpy, root);
		XCloseDisplay(dpy);
		if (!failures)
			puts("X11 initial fullscreen-exit checks passed");
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	if (argc == 2 && !strcmp(argv[1], "fullscreen-message-formats")) {
		check_fullscreen_message_formats(dpy, root);
		XCloseDisplay(dpy);
		if (!failures)
			puts("X11 fullscreen message-format checks passed");
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	/* x11 fullscreen: client messages preserve state, geometry and floating mode */
	if (argc == 2 && !strcmp(argv[1], "fullscreen")) {
		check_fullscreen_messages(dpy, root);
		XCloseDisplay(dpy);
		if (!failures)
			puts("X11 fullscreen client-message checks passed");
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	/* x11 unmanage: ordinary unmap and destroy update the layout, focus and client list */
	if (argc == 2 && !strcmp(argv[1], "unmanage")) {
		check_unmanage(dpy, root);
		XCloseDisplay(dpy);
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	/* x11 malformed-geometry: client-set sizes are checked before dwm uses them */
	if (argc == 2 && !strcmp(argv[1], "malformed-geometry")) {
		check_malformed_geometry(dpy, root);
		XCloseDisplay(dpy);
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	/* transient.c maps its parent in the background; let dwm manage it first so
	 * the tiled window below is the newest, selected client for the bar checks */
	floating = wait_named_window(dpy, root, "floating", 80);
	CHECK(floating != None, "production transient.c did not create its parent window");
	floating_managed = floating != None && wait_viewable(dpy, floating, 40);

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

	if (floating != None) {
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
