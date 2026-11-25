#define _POSIX_C_SOURCE 200809L
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xproto.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xinerama.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#include "../sysmon.h"

static int failures;
static int (*default_xerror)(Display *, XErrorEvent *);
/* The switcher key held with Ctrl; switcher-key checks another configured key. */
static KeySym opener = XK_Escape;

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

static Window
active_window(Display *dpy, Window root)
{
	Atom property = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	Atom type;
	int format;
	unsigned long count, remaining;
	unsigned char *data = NULL;
	Window active = None;

	if (XGetWindowProperty(dpy, root, property, 0, 1, False, XA_WINDOW,
	                       &type, &format, &count, &remaining, &data) == Success
	&& type == XA_WINDOW && format == 32 && count == 1)
		active = *(Window *)data;
	if (data)
		XFree(data);
	return active;
}

static int
wm_running(Display *dpy, Window root)
{
	Atom property = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
	Atom type;
	int format;
	unsigned long count, remaining;
	unsigned char *data = NULL;
	int found;

	found = XGetWindowProperty(dpy, root, property, 0, 1, False, XA_WINDOW,
	                           &type, &format, &count, &remaining, &data) == Success
	     && type == XA_WINDOW && format == 32 && count == 1;
	if (data)
		XFree(data);
	return found;
}

static int
wait_active(Display *dpy, Window root, Window target)
{
	int i;

	for (i = 0; i < 80; i++) {
		if (active_window(dpy, root) == target)
			return 1;
		pause_millis(25);
	}
	return 0;
}

static Window
create_client(Display *dpy, Window root, const char *name, int delete_protocol)
{
	Window win = XCreateSimpleWindow(dpy, root, 40, 40, 180, 120, 0, 0, 0);
	XClassHint class_hint = { "dwm-input-regression", "DwmInputRegression" };
	Atom protocol = XInternAtom(dpy, "WM_DELETE_WINDOW", False);

	XStoreName(dpy, win, name);
	XSetClassHint(dpy, win, &class_hint);
	XSelectInput(dpy, win, StructureNotifyMask | ButtonPressMask | KeyPressMask);
	if (delete_protocol)
		XSetWMProtocols(dpy, win, &protocol, 1);
	XMapWindow(dpy, win);
	XFlush(dpy);
	return win;
}

static int
count_delete_requests(Display *dpy, Window win, int duration)
{
	Atom protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
	Atom delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	int elapsed, count = 0;

	for (elapsed = 0; elapsed < duration; elapsed += 10) {
		XEvent event;
		while (XPending(dpy)) {
			XNextEvent(dpy, &event);
			if (event.type == ClientMessage && event.xclient.window == win
			&& event.xclient.message_type == protocols
			&& (Atom)event.xclient.data.l[0] == delete)
				count++;
		}
		pause_millis(10);
	}
	return count;
}

static int
wait_for_input(Display *dpy, Window win, int type, unsigned int detail, unsigned int ctrl)
{
	int elapsed;

	for (elapsed = 0; elapsed < 1000; elapsed += 10) {
		XEvent event;
		while (XPending(dpy)) {
			XNextEvent(dpy, &event);
			if (event.xany.window == win && event.type == type
			&& ((type == ButtonPress && event.xbutton.button == detail)
			 || (type == KeyPress && event.xkey.keycode == detail
			                      && (event.xkey.state & ControlMask) == ctrl)))
				return 1;
		}
		pause_millis(10);
	}
	return 0;
}

static int
test_idle_close(Display *dpy, Window root, KeyCode supercode, KeyCode qcode)
{
	Window client = create_client(dpy, root, "dwm-input-close", 1);
	int delivered;

	CHECK(wait_active(dpy, root, client), "close test client did not become selected");
	XTestFakeKeyEvent(dpy, supercode, True, CurrentTime);
	XTestFakeKeyEvent(dpy, qcode, True, CurrentTime);
	XTestFakeKeyEvent(dpy, qcode, False, CurrentTime);
	XTestFakeKeyEvent(dpy, supercode, False, CurrentTime);
	XFlush(dpy);

	/* No more input or redraw is generated during this wait. */
	delivered = count_delete_requests(dpy, client, 750);
	CHECK(delivered == 1,
	      "idle Mod+q delivered %d WM_DELETE_WINDOW requests (expected one)", delivered);
	XDestroyWindow(dpy, client);
	XFlush(dpy);
	return delivered == 1;
}

static int
test_click_replay(Display *dpy, Window root, int screen, KeyCode supercode,
                  KeyCode jcode, KeyCode acode)
{
	Window unfocused = create_client(dpy, root, "dwm-input-unfocused", 0);
	Window focused;
	XWindowAttributes attributes;
	Window child;
	int screen_x, screen_y, got_click = 0, got_key = 0;

	CHECK(wait_active(dpy, root, unfocused), "first click test client did not become selected");
	focused = create_client(dpy, root, "dwm-input-focused", 0);
	CHECK(wait_active(dpy, root, focused), "second click test client did not become selected");

	if (!XGetWindowAttributes(dpy, unfocused, &attributes)) {
		CHECK(0, "could not read the unfocused client geometry");
		return 0;
	}
	XTranslateCoordinates(dpy, unfocused, root, attributes.width / 2, attributes.height / 2,
	                      &screen_x, &screen_y, &child);
	XTestFakeMotionEvent(dpy, screen, screen_x, screen_y, CurrentTime);
	XFlush(dpy);
	CHECK(wait_active(dpy, root, unfocused), "pointer entry did not focus the first client");

	/* Move focus away while leaving the pointer over the first client. */
	XTestFakeKeyEvent(dpy, supercode, True, CurrentTime);
	XTestFakeKeyEvent(dpy, jcode, True, CurrentTime);
	XTestFakeKeyEvent(dpy, jcode, False, CurrentTime);
	XTestFakeKeyEvent(dpy, supercode, False, CurrentTime);
	XFlush(dpy);
	CHECK(wait_active(dpy, root, focused), "Mod+j did not focus the second client");

	/* Wait for replay before injecting typing; neither step wakes dwm. */
	XTestFakeButtonEvent(dpy, Button1, True, CurrentTime);
	XTestFakeButtonEvent(dpy, Button1, False, CurrentTime);
	XFlush(dpy);
	got_click = wait_for_input(dpy, unfocused, ButtonPress, Button1, 0);
	CHECK(got_click, "click on an unfocused client was not delivered");
	if (got_click) {
		XTestFakeKeyEvent(dpy, acode, True, CurrentTime);
		XTestFakeKeyEvent(dpy, acode, False, CurrentTime);
		XFlush(dpy);
		got_key = wait_for_input(dpy, unfocused, KeyPress, acode, 0);
		CHECK(got_key, "typing after the replayed click was not delivered");
	}

	XDestroyWindow(dpy, unfocused);
	XDestroyWindow(dpy, focused);
	XFlush(dpy);
	return got_click && got_key;
}

typedef struct {
	int x, y, w, h;
} Geometry;

static Geometry
geometry(Display *dpy, Window win)
{
	XWindowAttributes a;
	Geometry g = {0};

	if (XGetWindowAttributes(dpy, win, &a))
		g = (Geometry){a.x, a.y, a.width, a.height};
	return g;
}

static int
same_geometry(Geometry a, Geometry b)
{
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static int
wait_geometry(Display *dpy, Window win, Geometry expected)
{
	int i;

	for (i = 0; i < 80; i++) {
		if (same_geometry(geometry(dpy, win), expected))
			return 1;
		pause_millis(25);
	}
	return 0;
}

static void
key(Display *dpy, KeySym sym, KeySym modifier)
{
	KeyCode supercode = XKeysymToKeycode(dpy, XK_Super_L);
	KeyCode modcode = modifier ? XKeysymToKeycode(dpy, modifier) : 0;
	KeyCode code = XKeysymToKeycode(dpy, sym);

	XTestFakeKeyEvent(dpy, supercode, True, CurrentTime);
	if (modcode)
		XTestFakeKeyEvent(dpy, modcode, True, CurrentTime);
	XTestFakeKeyEvent(dpy, code, True, CurrentTime);
	XTestFakeKeyEvent(dpy, code, False, CurrentTime);
	if (modcode)
		XTestFakeKeyEvent(dpy, modcode, False, CurrentTime);
	XTestFakeKeyEvent(dpy, supercode, False, CurrentTime);
	XFlush(dpy);
}

static void
test_scratchpad_toggle(Display *dpy, Window root)
{
	Window scratch, normal;
	XClassHint hint = { "spcalc", "St" };
	Atom property = XInternAtom(dpy, "_NET_CLIENT_INFO", False), type;
	int format, i, managed = 0;
	unsigned long count, remaining;
	unsigned char *data;

	normal = create_client(dpy, root, "scratchpad normal client", 0);
	CHECK(wait_active(dpy, root, normal), "scratchpad test normal client was not selected");
	scratch = XCreateSimpleWindow(dpy, root, 40, 40, 180, 120, 0, 0, 0);
	XSetClassHint(dpy, scratch, &hint);
	XStoreName(dpy, scratch, "scratchpad regression");
	XMapWindow(dpy, scratch);
	XFlush(dpy);
	for (i = 0; i < 80; i++) {
		data = NULL;
		if (XGetWindowProperty(dpy, scratch, property, 0, 2, False, XA_CARDINAL,
		                       &type, &format, &count, &remaining, &data) == Success
		&& type == XA_CARDINAL && format == 32 && count == 2
		&& ((unsigned long *)data)[0] == (1UL << 10))
			managed = 1;
		if (data)
			XFree(data);
		if (managed)
			break;
		pause_millis(25);
	}
	CHECK(managed, "scratchpad client was not managed with its tag");
	if (!managed) {
		XDestroyWindow(dpy, scratch);
		XDestroyWindow(dpy, normal);
		return;
	}
	CHECK(wait_active(dpy, root, normal), "hidden scratchpad changed focus");
	key(dpy, XK_v, XK_Shift_L); /* push the normal client ahead of the scratchpad */
	key(dpy, XK_apostrophe, 0);
	CHECK(wait_active(dpy, root, scratch), "scratchpad did not show and gain focus (active %#lx)", active_window(dpy, root));
	key(dpy, XK_apostrophe, 0);
	CHECK(wait_active(dpy, root, normal), "scratchpad did not hide and restore focus");
	key(dpy, XK_apostrophe, 0);
	CHECK(wait_active(dpy, root, scratch), "scratchpad did not show again (active %#lx)", active_window(dpy, root));
	key(dpy, XK_apostrophe, 0);
	CHECK(wait_active(dpy, root, normal), "scratchpad did not hide again");
	XDestroyWindow(dpy, scratch);
	XDestroyWindow(dpy, normal);
	XFlush(dpy);
}

static int
bar_title_ink(Display *dpy, Window bar, int height, unsigned long background)
{
	XImage *image = XGetImage(dpy, bar, 250, 0, 150, height, AllPlanes, ZPixmap);
	int x, y, ink = 0;

	if (!image)
		return -1;
	for (y = 0; y < height; y++)
		for (x = 0; x < 150; x++)
			ink += XGetPixel(image, x, y) != background;
	XDestroyImage(image);
	return ink;
}

/* The empty view must clear the selected client's title from the bar. */
static void
test_empty_title(Display *dpy, Window root)
{
	Window parent, *children = NULL, bar = None, client;
	XWindowAttributes a;
	XColor background, exact;
	unsigned int count, j;
	char title[201];
	int i, ink = 0;

	if (!XQueryTree(dpy, root, &root, &parent, &children, &count)) {
		CHECK(0, "could not find the bar for the empty title check");
		return;
	}
	for (j = 0; j < count; j++)
		if (XGetWindowAttributes(dpy, children[j], &a) && a.override_redirect
		&& a.map_state == IsViewable && a.x == 0 && a.y == 0
		&& a.width == DisplayWidth(dpy, DefaultScreen(dpy)) && a.height < 40) {
			bar = children[j];
			break;
		}
	if (children)
		XFree(children);
	if (bar == None || !XAllocNamedColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)),
	                                    "#282a36", &background, &exact)) {
		CHECK(0, "could not prepare the empty title check");
		return;
	}
	memset(title, '_', sizeof title - 1);
	title[sizeof title - 1] = '\0';
	client = create_client(dpy, root, title, 0);
	CHECK(wait_active(dpy, root, client), "title test client was not selected");
	for (i = 0; i < 80; i++) {
		ink = bar_title_ink(dpy, bar, a.height, background.pixel);
		if (ink > 0)
			break;
		pause_millis(25);
	}
	CHECK(ink > 0, "selected title was not drawn in the bar");
	key(dpy, XK_9, 0);
	CHECK(wait_active(dpy, root, None), "empty tag retained an active client");
	for (i = 0; i < 80; i++) {
		ink = bar_title_ink(dpy, bar, a.height, background.pixel);
		if (ink == 0)
			break;
		pause_millis(25);
	}
	CHECK(ink == 0, "empty tag left %d title pixels in the bar", ink);
	key(dpy, XK_1, 0);
	XDestroyWindow(dpy, client);
	XFreeColors(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), &background.pixel, 1, 0);
	XFlush(dpy);
}

static void
test_pertag_views(Display *dpy, Window root)
{
	Window a, b, c, d;
	Geometry a1, b1, c2, d2;
	int i;

	a = create_client(dpy, root, "tag-one-a", 0);
	CHECK(wait_active(dpy, root, a), "first tag-one client was not selected");
	b = create_client(dpy, root, "tag-one-b", 0);
	CHECK(wait_active(dpy, root, b), "second tag-one client was not selected");
	key(dpy, XK_t, 0); /* tile, two masters, hidden bar */
	key(dpy, XK_o, 0);
	key(dpy, XK_b, 0);
	for (i = 0; i < 80; i++) {
		a1 = geometry(dpy, a);
		b1 = geometry(dpy, b);
		if (a1.w > 0 && a1.x == b1.x && a1.y != b1.y
		&& (a1.y == 10 || b1.y == 10))
			break;
		pause_millis(25);
	}
	CHECK(i < 80, "tag one did not arrange two masters vertically without a bar");

	key(dpy, XK_2, 0);
	c = create_client(dpy, root, "tag-two-c", 0);
	CHECK(wait_active(dpy, root, c), "first tag-two client was not selected");
	d = create_client(dpy, root, "tag-two-d", 0);
	CHECK(wait_active(dpy, root, d), "second tag-two client was not selected");
	key(dpy, XK_u, XK_Shift_L); /* monocle, visible bar */
	for (i = 0; i < 80; i++) {
		c2 = geometry(dpy, c);
		d2 = geometry(dpy, d);
		if (same_geometry(c2, d2) && c2.y > 10)
			break;
		pause_millis(25);
	}
	CHECK(i < 80, "tag two did not use monocle with the bar visible");

	key(dpy, XK_1, 0);
	CHECK(wait_geometry(dpy, a, a1) && wait_geometry(dpy, b, b1),
	      "view did not restore tag-one layout, master count and bar");
	key(dpy, XK_2, 0);
	CHECK(wait_geometry(dpy, c, c2) && wait_geometry(dpy, d, d2),
	      "view did not restore tag-two layout and bar");
	key(dpy, XK_1, 0);
	CHECK(wait_geometry(dpy, a, a1) && wait_geometry(dpy, b, b1),
	      "view did not return to tag one");
	key(dpy, XK_2, XK_Control_L); /* include tag two, keep tag one current */
	key(dpy, XK_1, XK_Control_L); /* remove tag one, select tag two */
	CHECK(wait_geometry(dpy, c, c2) && wait_geometry(dpy, d, d2),
	      "toggleview did not restore tag-two layout and bar");
	key(dpy, XK_1, XK_Control_L);
	key(dpy, XK_2, XK_Control_L);
	CHECK(wait_geometry(dpy, a, a1) && wait_geometry(dpy, b, b1),
	      "toggleview did not restore tag-one layout, master count and bar");

	/* Leave both tags with their default layout slots and the bar shown. */
	key(dpy, XK_o, XK_Shift_L);
	key(dpy, XK_b, 0);
	key(dpy, XK_z, XK_Shift_L);
	key(dpy, XK_t, XK_Shift_L);
	key(dpy, XK_z, XK_Shift_L);
	key(dpy, XK_2, 0);
	key(dpy, XK_z, XK_Shift_L);
	key(dpy, XK_t, XK_Shift_L);
	key(dpy, XK_z, XK_Shift_L);
	key(dpy, XK_1, 0);
	XDestroyWindow(dpy, a);
	XDestroyWindow(dpy, b);
	XDestroyWindow(dpy, c);
	XDestroyWindow(dpy, d);
	XFlush(dpy);
}

static int
switcher_xerror(Display *dpy, XErrorEvent *e)
{
	/* A panel may disappear between the root tree snapshot and the query. */
	if ((e->error_code == BadWindow && (e->request_code == X_GetWindowAttributes
	                                || e->request_code == X_GetGeometry))
	|| (e->error_code == BadDrawable && e->request_code == X_GetImage))
		return 0;
	return default_xerror(dpy, e);
}

static void
switcher_key(Display *dpy, KeySym sym, Bool down)
{
	XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, sym), down, CurrentTime);
	XFlush(dpy);
}

static void
tap_keycode(Display *dpy, KeyCode code)
{
	XTestFakeKeyEvent(dpy, code, True, CurrentTime);
	XTestFakeKeyEvent(dpy, code, False, CurrentTime);
	XFlush(dpy);
}

static void
switcher_tap(Display *dpy, KeySym sym)
{
	tap_keycode(dpy, XKeysymToKeycode(dpy, sym));
}

static Window
switcher_panel(Display *dpy, Window root, Geometry *outer)
{
	Window parent, *children = NULL, panel = None;
	XWindowAttributes a;
	unsigned int count, i;

	if (XQueryTree(dpy, root, &root, &parent, &children, &count)) {
		for (i = 0; i < count; i++) {
			/* The test server's bars span their heads and start at y <= 0. */
			if (XGetWindowAttributes(dpy, children[i], &a) && a.override_redirect
			&& a.map_state == IsViewable && a.y > 0) {
				panel = children[i];
				*outer = (Geometry){a.x, a.y, a.width + 2 * a.border_width,
				                                 a.height + 2 * a.border_width};
				break;
			}
		}
		if (children)
			XFree(children);
	}
	return panel;
}

static int
wait_panel(Display *dpy, Window root, int shown, Geometry *outer)
{
	int i;

	for (i = 0; i < 80; i++) {
		if ((switcher_panel(dpy, root, outer) != None) == shown)
			return 1;
		pause_millis(25);
	}
	return 0;
}

static int
open_switcher(Display *dpy, Window root, KeySym ctrl, Geometry *outer)
{
	switcher_key(dpy, ctrl, True);
	switcher_tap(dpy, opener);
	return wait_panel(dpy, root, 1, outer);
}

/* With Ctrl held, Shift plus the opener cancels the panel. */
static void
cancel_switcher(Display *dpy)
{
	switcher_key(dpy, XK_Shift_L, True);
	switcher_tap(dpy, opener);
	switcher_key(dpy, XK_Shift_L, False);
}

static int
has_opacity(Display *dpy, Window win, long long expected)
{
	Atom opacity = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False), type;
	int format, matches = 0;
	unsigned long count, remaining;
	unsigned char *data = NULL;

	if (XGetWindowProperty(dpy, win, opacity, 0, 1, False, XA_CARDINAL,
	                       &type, &format, &count, &remaining, &data) == Success)
		matches = expected < 0 ? type == None
		        : type == XA_CARDINAL && format == 32 && count == 1 && !remaining
		          && (*(unsigned long *)data & 0xffffffffUL) == (unsigned long)expected;
	if (data)
		XFree(data);
	return matches;
}

static int
wait_panel_style(Display *dpy, Window win, int rowheight, const char *fg, const char *bg,
                 long long opacity)
{
	Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
	XColor colors[2], exact;
	XImage *image;
	int i, matches = 0;

	if (!XAllocNamedColor(dpy, cmap, fg, &colors[0], &exact))
		return 0;
	if (!XAllocNamedColor(dpy, cmap, bg, &colors[1], &exact)) {
		XFreeColors(dpy, cmap, &colors[0].pixel, 1, 0);
		return 0;
	}
	for (i = 0; i < 80; i++) {
		/* Sample the border, then the ordinary and selected row backgrounds. */
		image = XGetImage(dpy, win, -1, 0, 2, rowheight + 1, AllPlanes, ZPixmap);
		if (!image)
			break;
		matches = XGetPixel(image, 0, 0) == colors[0].pixel
		       && XGetPixel(image, 1, 0) == colors[1].pixel
		       && XGetPixel(image, 1, rowheight) == colors[0].pixel
		       && has_opacity(dpy, win, opacity);
		XDestroyImage(image);
		if (matches)
			break;
		pause_millis(25);
	}
	for (i = 0; i < 2; i++)
		XFreeColors(dpy, cmap, &colors[i].pixel, 1, 0);
	return matches;
}

static void
set_resources(Display *dpy, Window root, const char *resources)
{
	XChangeProperty(dpy, root, XA_RESOURCE_MANAGER, XA_STRING, 8, PropModeReplace,
	                (const unsigned char *)resources, strlen(resources));
}

static const unsigned long appearance_client_opacity = 0x99999999UL;

static void
check_window_opacities(Display *dpy, Window root, Window panel, Window client,
                       long long baropacity, size_t stage)
{
	Window parent, *children = NULL;
	unsigned int i, count, bars = 0;

	CHECK(XQueryTree(dpy, root, &root, &parent, &children, &count),
	      "could not inspect non-switcher windows");
	if (children) {
		for (i = 0; i < count; i++) {
			XClassHint hint;
			long long expected = children[i] == client ? (long long)appearance_client_opacity : -1;

			if (children[i] == panel)
				continue;
			if (XGetClassHint(dpy, children[i], &hint)) {
				if (!strcmp(hint.res_class, "dwm") && !strcmp(hint.res_name, "dwm")) {
					expected = baropacity;
					bars++;
				}
				XFree(hint.res_name);
				XFree(hint.res_class);
			}
			CHECK(has_opacity(dpy, children[i], expected),
			      "unexpected opacity for window %#lx at stage %zu", children[i], stage);
		}
		XFree(children);
	}
	CHECK(bars == 2, "could not verify both bars at stage %zu", stage);
}

static void
test_switcher_appearance(Display *dpy, Window root, pid_t dwm_pid)
{
	enum { Dark, Failed, Light };
	static const char *const themes[] = {
		[Dark] = "dwm.color0: #102030\ndwm.color4: #d0c0a0\n",
		[Failed] = "dwm.color0: #301020\ndwm.color4: #a0d0c0\n"
		           "dwm.borderpx: 3\ndwm.selbordercolor: #fffffg\n",
		[Light] = "dwm.color0: #f8f8f2\ndwm.color4: #555b6e\n",
	};
	static const struct {
		int theme;
		const char *resources, *fg, *bg;
		long long opacity, baropacity;
	} styles[] = {
		{ Dark, NULL, "#fedcba", "#563412", 0xffffffffUL, -1 },
		{ Dark, "dwm.switcherfgcolor: #bogus!\ndwm.switcheropacity: 40junk\n",
		  "#d0c0a0", "#102030", -1, -1 },
		{ Dark, "dwm.switcherfgcolor: #123456\ndwm.switcherbgcolor: #abcdef\n"
		        "dwm.switcheropacity: 80\ndwm.baropacity: 90\n",
		  "#123456", "#abcdef", 0xccccccccUL, 0xe6666665UL },
		{ Dark, "dwm.switcherfgcolor: #123456\ndwm.switcherbgcolor: #abcdef\n"
		        "dwm.switcheropacity: 100\ndwm.baropacity: 0\n",
		  "#123456", "#abcdef", 0xffffffffUL, -1 },
		{ Failed, "dwm.switcheropacity: 80\n", "#123456", "#abcdef", 0xffffffffUL, -1 },
		{ Failed, "dwm.switcheropacity: 80\n", "#d0c0a0", "#102030", 0xccccccccUL, -1 },
		{ Failed, "dwm.switcherfgcolor:\ndwm.switcherbgcolor:\ndwm.switcheropacity:\n",
		  "#d0c0a0", "#102030", -1, -1 },
		{ Dark, "dwm.selbordercolor: #476a8c\ndwm.switcheropacity: 20\n",
		  "#d0c0a0", "#102030", 0x33333333UL, -1 },
		{ Dark, "", "#d0c0a0", "#102030", -1, -1 },
		{ Light, "dwm.baropacity: 75\n", "#555b6e", "#f6e8ee", -1, 0xeb851eb8UL },
		{ Light, "dwm.switcherfgcolor: #123456\ndwm.switcherbgcolor: #abcdef\n"
		         "dwm.switcheropacity: 80\ndwm.baropacity: 90\n",
		  "#123456", "#abcdef", 0xccccccccUL, 0xe6666665UL },
		{ Light, "dwm.switcherfgcolor:\ndwm.switcherbgcolor:\ndwm.switcheropacity:\n",
		  "#555b6e", "#f6e8ee", -1, 0xeb851eb8UL },
		{ Dark, "", "#d0c0a0", "#102030", -1, -1 },
	};
	Window first, second, child, focused, probe, win;
	Atom opacity = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
	Geometry panel = {0};
	XWindowAttributes a;
	int k, revert;
	size_t i;
	char database[256];

	first = create_client(dpy, root, "Appearance one", 0);
	CHECK(wait_active(dpy, root, first), "first appearance client not focused");
	XChangeProperty(dpy, first, opacity, XA_CARDINAL, 32, PropModeReplace,
	                (const unsigned char *)&appearance_client_opacity, 1);
	second = create_client(dpy, root, "Appearance two", 0);
	CHECK(wait_active(dpy, root, second), "second appearance client not focused");
	child = XCreateSimpleWindow(dpy, second, 0, 0, 8, 8, 0, 0, 0);
	XMapWindow(dpy, child);
	XSetInputFocus(dpy, child, RevertToParent, CurrentTime);
	probe = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
	if (!open_switcher(dpy, root, XK_Control_L, &panel) || !(win = switcher_panel(dpy, root, &panel))) {
		CHECK(0, "switcher did not stay open for the appearance check");
		goto close;
	}
	for (i = 0; i < sizeof styles / sizeof styles[0]; i++) {
		int recovering = styles[i].theme != Failed && i > 0 && styles[i - 1].theme == Failed;

		if (styles[i].resources) {
			snprintf(database, sizeof database, "%sdwm.showsysmon: 0\ndwm.smartgaps: 0\n%s",
			         themes[styles[i].theme], styles[i].resources);
			set_resources(dpy, root, database);
			XSync(dpy, False);
			CHECK(kill(dwm_pid, SIGUSR2) == 0, "could not signal the test dwm");
		}
		if (styles[i].theme == Failed || recovering) {
			const int width = styles[i].theme == Failed ? 3 : 2;

			for (k = 0; k < 80; k++) {
				if (XGetWindowAttributes(dpy, first, &a) && a.border_width == width
				&& XGetWindowAttributes(dpy, second, &a) && a.border_width == width)
					break;
				pause_millis(25);
			}
			CHECK(k < 80, "both client borders should be %d pixels at stage %zu", width, i);
		}
		CHECK(wait_panel_style(dpy, win, (panel.h - 2) / 2, styles[i].fg, styles[i].bg,
		                       styles[i].opacity), "wrong switcher appearance at stage %zu", i);
		XResizeWindow(dpy, probe, i + 2, 1);
		CHECK(wait_geometry(dpy, probe, (Geometry){0, 0, i + 2, 1}),
		      "dwm did not acknowledge configure request at stage %zu", i);
		XGetInputFocus(dpy, &focused, &revert);
		CHECK(focused == child && active_window(dpy, root) == second,
		      "appearance change disturbed focus at stage %zu", i);
		if (recovering) {
			Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
			XColor border, exact;
			XImage *image;

			if (XAllocNamedColor(dpy, cmap, "#476a8c", &border, &exact)) {
				image = XGetImage(dpy, second, -1, 0, 1, 1, AllPlanes, ZPixmap);
				CHECK(image && XGetPixel(image, 0, 0) == border.pixel,
				      "theme recovery did not repaint the selected client border");
				if (image)
					XDestroyImage(image);
				XFreeColors(dpy, cmap, &border.pixel, 1, 0);
			} else {
				CHECK(0, "could not allocate the recovery border color");
			}
		}
		if (styles[i].theme == Failed) {
			cancel_switcher(dpy);
			switcher_key(dpy, XK_Control_L, False);
			if (!wait_panel(dpy, root, 0, &panel) || !open_switcher(dpy, root, XK_Control_L, &panel)
			|| !(win = switcher_panel(dpy, root, &panel))) {
				CHECK(0, "could not reopen after the failed theme at stage %zu", i);
				goto close;
			}
			CHECK(wait_panel_style(dpy, win, (panel.h - 2) / 2, styles[i].fg, styles[i].bg,
			                       styles[i].opacity), "wrong reopened appearance at stage %zu", i);
		}
		check_window_opacities(dpy, root, win, first, styles[i].baropacity, i);
	}
close:
	cancel_switcher(dpy);
	switcher_key(dpy, XK_Control_L, False);
	CHECK(wait_panel(dpy, root, 0, &panel), "cancel did not dismiss the reloaded panel");
	XDestroyWindow(dpy, probe);
	XDestroyWindow(dpy, first);
	XDestroyWindow(dpy, second);
	CHECK(wait_active(dpy, root, None), "appearance clients did not clear focus");
}

static void
wait_sysmon(Display *dpy, Window root, unsigned long expected)
{
	Atom property = XInternAtom(dpy, SYSMON_ENABLED_ATOM, False), type;
	unsigned long count, remaining;
	unsigned char *data;
	int format, i, matches;

	for (i = 0; i < 80; i++) {
		data = NULL;
		matches = XGetWindowProperty(dpy, root, property, 0, 1, False, XA_CARDINAL,
		                            &type, &format, &count, &remaining, &data) == Success
		       && type == XA_CARDINAL && format == 32 && count == 1
		       && *(unsigned long *)data == expected;
		if (data)
			XFree(data);
		if (matches)
			return;
		pause_millis(25);
	}
	CHECK(0, "sysmon did not reach state %lu", expected);
}

static void
check_reopened_switcher_opacity(Display *dpy, Window root, unsigned long opacity)
{
	Geometry panel = {0};
	Window win;

	CHECK(open_switcher(dpy, root, XK_Control_L, &panel)
	   && (win = switcher_panel(dpy, root, &panel)) && has_opacity(dpy, win, opacity),
	      "next switcher panel did not use opacity %#lx", opacity);
	cancel_switcher(dpy);
	switcher_key(dpy, XK_Control_L, False);
	CHECK(wait_panel(dpy, root, 0, &panel), "cancel did not dismiss the reopened panel");
}

static void
test_switcher_closed_reload(Display *dpy, Window root, pid_t dwm_pid)
{
	static const char resources[] = "dwm.color0: #102030\ndwm.color4: #d0c0a0\n"
	                                "dwm.showsysmon: 0\ndwm.smartgaps: 0\ndwm.switcheropacity: 90\n";
	static const char ordinary[] = "dwm.color0: #102031\ndwm.color4: #d0c0a0\n"
	                               "dwm.switcheropacity: %d\ndwm.smartgaps: 0\ndwm.showsysmon: %d\n%s";
	static const char baroverride[] = "dwm.baropacity: 90\n";
	static const struct {
		int switcherpercent, override;
	} reloads[] = { {90, 0}, {90, 0}, {90, 1}, {80, 0}, {90, 1} };
	const Geometry moved = {30, 60, 180, 120};
	XClassHint hint = { "spterm", "St" };
	Window tile, pad, child, focused, probe;
	Geometry gapped, gapless;
	int i, revert;
	size_t stage;
	char database[sizeof ordinary + sizeof baroverride];

	wait_sysmon(dpy, root, 0);
	key(dpy, XK_b, XK_Shift_L);
	wait_sysmon(dpy, root, 1);
	XWarpPointer(dpy, None, root, 0, 0, 0, 0, 1, 1);
	tile = create_client(dpy, root, "Smart gaps reload fixture", 0);
	CHECK(wait_active(dpy, root, tile), "smart-gaps fixture did not gain focus");
	gapped = geometry(dpy, tile);
	key(dpy, XK_apostrophe, XK_Shift_L);
	for (i = 0; i < 80; i++) {
		gapless = geometry(dpy, tile);
		if (gapless.w > gapped.w && gapless.h > gapped.h)
			break;
		pause_millis(25);
	}
	CHECK(i < 80, "smart gaps did not remove the tiled window's outer gaps");
	pad = XCreateSimpleWindow(dpy, root, 40, 40, moved.w, moved.h, 0, 0, 0);
	XSetClassHint(dpy, pad, &hint);
	XMapWindow(dpy, pad);
	for (i = 0; i < 80 && geometry(dpy, pad).x >= 0; i++)
		pause_millis(25);
	CHECK(i < 80, "reload scratchpad did not start hidden");
	if (i == 80)
		goto close;
	key(dpy, XK_Return, XK_Shift_L);
	CHECK(wait_active(dpy, root, pad), "reload scratchpad did not gain focus");
	XMoveWindow(dpy, pad, moved.x, moved.y);
	CHECK(wait_geometry(dpy, pad, moved), "could not move the visible scratchpad");
	child = XCreateSimpleWindow(dpy, pad, 0, 0, 8, 8, 0, 0, 0);
	XMapWindow(dpy, child);
	XSetInputFocus(dpy, child, RevertToParent, CurrentTime);
	XGetInputFocus(dpy, &focused, &revert);
	CHECK(focused == child, "could not focus the scratchpad child window");
	probe = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
	set_resources(dpy, root, resources);
	key(dpy, XK_F5, 0);
	XResizeWindow(dpy, probe, 2, 1);
	CHECK(wait_geometry(dpy, probe, (Geometry){0, 0, 2, 1}), "dwm did not finish the closed-panel reload");
	XGetInputFocus(dpy, &focused, &revert);
	CHECK(focused == child && active_window(dpy, root) == pad, "closed-panel reload changed focus");
	CHECK(same_geometry(geometry(dpy, pad), moved), "closed-panel reload moved the scratchpad");
	CHECK(same_geometry(geometry(dpy, tile), gapless), "closed-panel reload restored outer gaps");
	wait_sysmon(dpy, root, 1);
	check_reopened_switcher_opacity(dpy, root, 0xe6666665UL);
	key(dpy, XK_z, XK_Shift_L);
	XResizeWindow(dpy, probe, 3, 1);
	CHECK(wait_geometry(dpy, probe, (Geometry){0, 0, 3, 1})
	   && !same_geometry(geometry(dpy, pad), moved), "layout pass did not recenter the scratchpad");
	CHECK(same_geometry(geometry(dpy, tile), gapless), "layout pass restored outer gaps after switcher reload");
	key(dpy, XK_b, XK_Shift_L);
	wait_sysmon(dpy, root, 0);
	key(dpy, XK_b, XK_Shift_L);
	wait_sysmon(dpy, root, 1);
	for (i = 0; i < 2; i++) {
		snprintf(database, sizeof database, ordinary, 90, i, "");
		set_resources(dpy, root, database);
		key(dpy, XK_F5, 0);
		wait_sysmon(dpy, root, i);
	}
	CHECK(wait_geometry(dpy, tile, gapped), "ordinary theme reload did not restore outer gaps");
	for (stage = 0; stage < sizeof reloads / sizeof reloads[0]; stage++) {
		int width = (int)stage + 4;
		int switcherpercent = reloads[stage].switcherpercent;
		int override = reloads[stage].override;

		key(dpy, XK_b, XK_Shift_L);
		wait_sysmon(dpy, root, 0);
		XMoveWindow(dpy, pad, moved.x, moved.y);
		CHECK(wait_geometry(dpy, pad, moved), "could not move the scratchpad before ordinary reload %zu", stage);
		XSetInputFocus(dpy, child, RevertToParent, CurrentTime);
		XGetInputFocus(dpy, &focused, &revert);
		CHECK(focused == child, "could not focus the scratchpad child before ordinary reload %zu", stage);
		if (stage) {
			snprintf(database, sizeof database, ordinary, switcherpercent, 1, override ? baroverride : "");
			set_resources(dpy, root, database);
		}
		XSync(dpy, False);
		CHECK(kill(dwm_pid, SIGUSR2) == 0, "could not signal ordinary reload %zu", stage);
		wait_sysmon(dpy, root, 1);
		XResizeWindow(dpy, probe, width, 1);
		CHECK(wait_geometry(dpy, probe, (Geometry){0, 0, width, 1}),
		      "dwm did not finish ordinary reload %zu", stage);
		XGetInputFocus(dpy, &focused, &revert);
		CHECK(focused == pad && !same_geometry(geometry(dpy, pad), moved),
		      "ordinary reload %zu did not refresh scratchpad focus and layout", stage);
		check_window_opacities(dpy, root, None, None, override ? 0xe6666665LL : -1, stage);
		check_reopened_switcher_opacity(dpy, root, switcherpercent == 80 ? 0xccccccccUL : 0xe6666665UL);
	}
	set_resources(dpy, root, resources);
	key(dpy, XK_F5, 0);
	wait_sysmon(dpy, root, 0);
	XDestroyWindow(dpy, probe);
	key(dpy, XK_Return, XK_Shift_L);
	CHECK(wait_active(dpy, root, tile), "reload scratchpad did not hide");
close:
	XDestroyWindow(dpy, pad);
	XDestroyWindow(dpy, tile);
	CHECK(wait_active(dpy, root, None), "reload scratchpad did not clear focus");
}

static void
queued_switch(Display *dpy, KeySym ctrl, KeySym step, int steps, Window target)
{
	XEvent e;
	KeyCode printable = XKeysymToKeycode(dpy, XK_x);
	int i;

	XSync(dpy, True);
	XGrabServer(dpy);
	switcher_key(dpy, ctrl, True);
	switcher_tap(dpy, opener);
	while (steps-- > 0)
		switcher_tap(dpy, step);
	switcher_key(dpy, ctrl, False);
	switcher_tap(dpy, XK_x);
	XUngrabServer(dpy);
	XFlush(dpy);
	for (i = 0; i < 80; i++) {
		while (XCheckMaskEvent(dpy, KeyPressMask, &e)) {
			CHECK(!step || e.xkey.keycode != XKeysymToKeycode(dpy, step),
			      "queued navigation reached a client");
			if (e.xkey.keycode == printable) {
				CHECK(e.xkey.window == target && !(e.xkey.state & ControlMask),
				      "queued x went to %#lx instead of %#lx", e.xkey.window, target);
				return;
			}
		}
		pause_millis(25);
	}
	CHECK(0, "no client received queued x after activation");
}

static int
wait_highlight(Display *dpy, Window panel, int height, int rowheight, int selected)
{
	XImage *image;
	unsigned long background;
	int i, y;

	for (i = 0; panel && rowheight > 0 && height > rowheight && i < 80; i++) {
		image = XGetImage(dpy, panel, 0, 0, 1, height, AllPlanes, ZPixmap);
		if (!image)
			break;
		background = XGetPixel(image, 0, selected ? 0 : rowheight);
		for (y = 0; y < height; y++)
			if ((XGetPixel(image, 0, y) != background) != (y / rowheight == selected))
				break;
		XDestroyImage(image);
		if (y == height)
			return 1;
		pause_millis(25);
	}
	return 0;
}

static int
panel_on(Geometry panel, XineramaScreenInfo head)
{
	return panel.w > 0 && panel.h > 0
	    && abs(2 * (panel.x - head.x_org) + panel.w - head.width) <= 1
	    && abs(2 * (panel.y - head.y_org) + panel.h - head.height) <= 1
	    && panel.w <= head.width && panel.h <= head.height;
}

static Window
switcher_client(Display *dpy, Window root, const char *title,
                unsigned long tags, unsigned long monitor)
{
	unsigned long info[] = { tags, monitor };
	Atom clientinfo = XInternAtom(dpy, "_NET_CLIENT_INFO", False);
	Window win = XCreateSimpleWindow(dpy, root, 40, 40, 180, 120, 0, 0, 0);

	XStoreName(dpy, win, title);
	XSelectInput(dpy, win, KeyPressMask);
	XChangeProperty(dpy, win, clientinfo, XA_CARDINAL, 32, PropModeReplace,
	                (const unsigned char *)info, 2);
	XMapWindow(dpy, win);
	return win;
}

static int
wait_visible(Display *dpy, Window win, XineramaScreenInfo head, int visible)
{
	XWindowAttributes a;
	int i;

	for (i = 0; i < 80; i++) {
		if (XGetWindowAttributes(dpy, win, &a) && a.map_state == IsViewable
		&& (a.x >= head.x_org && a.x < head.x_org + head.width) == visible)
			return 1;
		pause_millis(25);
	}
	return 0;
}

static int
clientinfo_is(Display *dpy, Window win, unsigned long tags, unsigned long monitor)
{
	Atom property = XInternAtom(dpy, "_NET_CLIENT_INFO", False), type;
	unsigned char *data = NULL;
	unsigned long n, remaining;
	int format, matches = 0;

	if (XGetWindowProperty(dpy, win, property, 0, 2, False, XA_CARDINAL,
	                       &type, &format, &n, &remaining, &data) == Success
	&& type == XA_CARDINAL && format == 32 && n == 2 && !remaining)
		matches = ((unsigned long *)data)[0] == tags && ((unsigned long *)data)[1] == monitor;
	if (data)
		XFree(data);
	return matches;
}

enum { ScratchTerm = 1U << 9, ScratchCalc = 1U << 10 };

static void
check_view(Display *dpy, Window root, Window previous, unsigned long tags,
           XineramaScreenInfo head, Window pads[2])
{
	Window probe = create_client(dpy, root, "view probe", 0);

	CHECK(wait_active(dpy, root, probe) && clientinfo_is(dpy, probe, tags & (ScratchTerm - 1), head.screen_number),
	      "new window did not inherit the ordinary tags of %#lx on monitor %d", tags, head.screen_number);
	for (int i = 0; i < 2; i++)
		CHECK(clientinfo_is(dpy, pads[i], ScratchTerm << i, head.screen_number)
		   && wait_visible(dpy, pads[i], head, !!(tags & (ScratchTerm << i))),
		      "scratchpad %d disagrees with view %#lx on monitor %d", i, tags, head.screen_number);
	XDestroyWindow(dpy, probe);
	CHECK(wait_active(dpy, root, previous), "removing view probe did not restore focus");
}

static void
record_layout(Display *dpy, Window first, Window second, Geometry *a, Geometry *b, int monocle)
{
	for (int i = 0; i < 80; i++) {
		*a = geometry(dpy, first);
		*b = geometry(dpy, second);
		if (a->w > 0 && b->w > 0 && (monocle ? same_geometry(*a, *b)
		    : a->x + a->w <= b->x || b->x + b->w <= a->x))
			return;
		pause_millis(25);
	}
	CHECK(0, "windows did not settle into the expected %s layout", monocle ? "monocle" : "tiled");
}

static int
above_window(Display *dpy, Window root, Window upper, Window lower)
{
	Window parent, *children = NULL;
	unsigned int n;
	int seen = 0, above = 0;

	if (XQueryTree(dpy, root, &root, &parent, &children, &n)) {
		for (unsigned int i = 0; i < n; i++) {
			if (children[i] == lower)
				seen = 1;
			if (children[i] == upper)
				above = seen;
		}
		XFree(children);
	}
	return above;
}

static void
check_floating_switcher(Display *dpy, Window root, Window parent, XineramaScreenInfo head)
{
	Window scratch, panelwin, dialog = XCreateSimpleWindow(dpy, root, 0, 0, 160, 100, 0, 0, 0);
	XEvent request = { .xclient = { .type = ClientMessage, .window = dialog,
		.message_type = XInternAtom(dpy, "_NET_WM_STATE", False), .format = 32 } };
	Geometry before, after, panel = {0};
	int i, button, drag;

	XSelectInput(dpy, dialog, KeyPressMask);
	XSetTransientForHint(dpy, dialog, parent);
	XMapWindow(dpy, dialog);
	CHECK(wait_active(dpy, root, dialog), "drag fixture was not focused");
	for (drag = 0; drag < 3; drag++) {
		button = drag == 1 ? Button1 : Button3;
		before = geometry(dpy, dialog);
		XWarpPointer(dpy, None, dialog, 0, 0, 0, 0, before.w / 2, before.h / 2);
		switcher_key(dpy, XK_Super_L, True);
		XTestFakeButtonEvent(dpy, button, True, CurrentTime);
		for (i = 0; i < 20; i++) {
			XTestFakeRelativeMotionEvent(dpy, 12, 8, 25);
			after = geometry(dpy, dialog);
			if (button == Button1 ? before.x != after.x && before.y != after.y
			                      : before.w != after.w && before.h != after.h)
				break;
		}
		CHECK(i < 20, "button %d did not drag the window", button);
		switcher_key(dpy, XK_Super_L, False);
		if (!drag) {
			XTestFakeButtonEvent(dpy, button, False, CurrentTime);
			switcher_tap(dpy, XK_x);
			CHECK(wait_for_input(dpy, dialog, KeyPress, XKeysymToKeycode(dpy, XK_x), 0)
			   && active_window(dpy, root) == dialog, "plain resize lost client focus");
			continue;
		}
		switcher_key(dpy, XK_Control_L, True);
		switcher_tap(dpy, opener);
		XTestFakeButtonEvent(dpy, button, False, CurrentTime);
		CHECK(wait_panel(dpy, root, 1, &panel), "queued opener blocked button %d release", button);
		cancel_switcher(dpy);
		switcher_key(dpy, XK_Control_L, False);
		CHECK(wait_panel(dpy, root, 0, &panel), "could not dismiss panel after drag");
	}
	scratch = switcher_client(dpy, root, "hidden restack witness", ScratchCalc, head.screen_number);
	CHECK(wait_visible(dpy, scratch, head, 0), "restack witness was not hidden");
	CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "could not open for floating restack");
	panelwin = switcher_panel(dpy, root, &panel);
	XDestroyWindow(dpy, scratch);
	XMoveResizeWindow(dpy, dialog, 20, 20, head.width - 40, head.height - 40);
	after = (Geometry){ head.x_org + 20, head.y_org + 20, head.width - 40, head.height - 40 };
	CHECK(wait_geometry(dpy, dialog, after) && panel.x >= after.x && panel.y >= after.y
	   && panel.x + panel.w <= after.x + after.w && panel.y + panel.h <= after.y + after.h,
	      "floating client did not overlap the panel after restacking");
	CHECK(above_window(dpy, root, panelwin, dialog), "floating restack covered the live panel");
	request.xclient.data.l[0] = 1;
	request.xclient.data.l[1] = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &request);
	CHECK(wait_geometry(dpy, dialog, (Geometry){ head.x_org, head.y_org, head.width, head.height })
	   && wait_panel(dpy, root, 0, &panel), "newly fullscreen origin kept the switcher armed");
	switcher_key(dpy, XK_Control_L, False);
	CHECK(active_window(dpy, root) == dialog, "fullscreen cancellation changed selection");
	XDestroyWindow(dpy, dialog);
	CHECK(wait_active(dpy, root, parent), "removing drag fixture did not restore focus");
}

static int
wait_configure_ack(Display *dpy, Window tiled)
{
	XEvent ev;
	int i;

	XResizeWindow(dpy, tiled, 1, 1);
	for (i = 0; i < 80; i++) {
		while (XCheckTypedWindowEvent(dpy, tiled, ConfigureNotify, &ev))
			if (ev.xconfigure.send_event)
				return 1;
		pause_millis(25);
	}
	return 0;
}

static int
wait_opener_release_handled(Display *dpy, Window tiled)
{
	KeyCode code = XKeysymToKeycode(dpy, opener);
	char keys[32];
	int i;

	for (i = 0; i < 80; i++) {
		XQueryKeymap(dpy, keys);
		if (!(keys[code / 8] & (1 << (code % 8))))
			return wait_configure_ack(dpy, tiled);
		pause_millis(25);
	}
	return 0;
}

static void
check_keyboard_scope(Display *dpy, Window root)
{
	XIAnyHierarchyChangeInfo change = { .add = { XIAddMaster, "switcher-test", True, True } };
	XIGrabModifiers mod = { ControlMask, 0 };
	unsigned char bits[XIMaskLen(XI_KeyPress)] = {0};
	XIEventMask mask = { 0, sizeof bits, bits };
	XIDeviceInfo *devices;
	XDevice keyboard = {0};
	Window probe, origin;
	Geometry panel = {0};
	KeyCode super = XKeysymToKeycode(dpy, XK_Super_L), j = XKeysymToKeycode(dpy, XK_j);
	int i, n, major = 2, minor = 0;

	if (XIQueryVersion(dpy, &major, &minor) != Success || XIChangeHierarchy(dpy, &change, 1) != Success) {
		CHECK(0, "could not create independent test keyboard");
		return;
	}
	probe = create_client(dpy, root, "keyboard scope probe", 0);
	CHECK(wait_active(dpy, root, probe), "keyboard scope probe was not managed");
	devices = XIQueryDevice(dpy, XIAllDevices, &n);
	for (i = 0; devices && i < n; i++) {
		if (!strcmp(devices[i].name, "switcher-test keyboard"))
			mask.deviceid = devices[i].deviceid;
		if (!strcmp(devices[i].name, "switcher-test XTEST keyboard"))
			keyboard.device_id = devices[i].deviceid;
	}
	if (devices)
		XIFreeDeviceInfo(devices);
	CHECK(mask.deviceid && keyboard.device_id, "independent test keyboard was not found");
	if (mask.deviceid) {
		XISetMask(bits, XI_KeyPress);
		CHECK(XIGrabKeycode(dpy, mask.deviceid, XKeysymToKeycode(dpy, opener), root, GrabModeAsync,
		                    GrabModeAsync, False, &mask, 1, &mod) == 0,
		      "dwm reserved the Ctrl opener on an independent keyboard");
		if (keyboard.device_id) {
			origin = create_client(dpy, root, "keyboard scope origin", 0);
			CHECK(wait_active(dpy, root, origin) && XISetFocus(dpy, mask.deviceid, origin, CurrentTime) == Success,
			      "could not focus the independent keyboard's origin");
			XSync(dpy, True);
			XTestFakeDeviceKeyEvent(dpy, &keyboard, super, True, NULL, 0, CurrentTime);
			XTestFakeDeviceKeyEvent(dpy, &keyboard, super, False, NULL, 0, CurrentTime);
			CHECK(wait_configure_ack(dpy, origin), "independent keyboard did not settle before opener");
			CHECK(open_switcher(dpy, root, XK_Control_L, &panel) && wait_opener_release_handled(dpy, origin)
			   && switcher_panel(dpy, root, &panel), "panel did not stay open before independent binding");
			XTestFakeDeviceKeyEvent(dpy, &keyboard, super, True, NULL, 0, CurrentTime);
			XTestFakeDeviceKeyEvent(dpy, &keyboard, j, True, NULL, 0, CurrentTime);
			XTestFakeDeviceKeyEvent(dpy, &keyboard, j, False, NULL, 0, CurrentTime);
			XTestFakeDeviceKeyEvent(dpy, &keyboard, super, False, NULL, 0, CurrentTime);
			CHECK(wait_active(dpy, root, probe) && wait_panel(dpy, root, 0, &panel),
			      "independent Super+j did not focus another client and cancel the switcher");
			switcher_key(dpy, XK_Control_L, False);
			XDestroyWindow(dpy, origin);
		}
		change.remove = (XIRemoveMasterInfo){ .type = XIRemoveMaster, .deviceid = mask.deviceid,
		                                     .return_mode = XIFloating };
		XIChangeHierarchy(dpy, &change, 1);
	}
	XDestroyWindow(dpy, probe);
	CHECK(wait_active(dpy, root, None), "keyboard scope probe retained focus");
}

static void
check_switcher_without_numlock(Display *dpy, Window root, Window origin)
{
	KeyCode numlock = XKeysymToKeycode(dpy, XK_Num_Lock);
	KeyCode altgr = XKeysymToKeycode(dpy, XK_ISO_Level3_Shift);
	KeyCode tab = XKeysymToKeycode(dpy, opener);
	KeySym replacement = XK_F12;
	Display *reader;
	XModifierKeymap *modmap;
	XkbStateRec state;
	Geometry panel = {0};
	int i, padded = 0;

	CHECK(numlock && altgr && tab, "Num Lock regression requires Num Lock, AltGr and opener keycodes");
	if (!numlock || !altgr || !tab)
		return;
	XChangeKeyboardMapping(dpy, numlock, 1, &replacement, 1);
	XSync(dpy, True);
	CHECK(wait_configure_ack(dpy, origin), "Num Lock mapping change was not acknowledged");
	reader = XOpenDisplay(DisplayString(dpy));
	CHECK(reader && !XKeysymToKeycode(reader, XK_Num_Lock), "test keymap still contains Num Lock");
	if (reader)
		XCloseDisplay(reader);
	if ((modmap = XGetModifierMapping(dpy))) {
		for (i = 0; i < modmap->max_keypermod; i++)
			padded |= modmap->modifiermap[Mod5MapIndex * modmap->max_keypermod + i] == 0;
		XFreeModifiermap(modmap);
	}
	CHECK(padded, "test keymap requires an empty slot in Mod5");
	switcher_key(dpy, XK_Control_L, True);
	XTestFakeKeyEvent(dpy, altgr, True, CurrentTime);
	CHECK(XkbGetState(dpy, XkbUseCoreKbd, &state) == Success
	   && (state.base_mods & (ControlMask | Mod5Mask)) == (ControlMask | Mod5Mask),
	      "AltGr did not activate Mod5 with Ctrl");
	tap_keycode(dpy, tab);
	CHECK(wait_for_input(dpy, origin, KeyPress, tab, ControlMask) && !switcher_panel(dpy, root, &panel),
	      "Ctrl+AltGr+opener did not reach the application without Num Lock");
	XTestFakeKeyEvent(dpy, altgr, False, CurrentTime);
	switcher_key(dpy, XK_Control_L, False);
	CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "plain Ctrl opener stopped working without Num Lock");
	cancel_switcher(dpy);
	switcher_key(dpy, XK_Control_L, False);
	CHECK(wait_panel(dpy, root, 0, &panel), "could not dismiss panel without Num Lock");
}

static void
check_switcher_group(Display *dpy, Window root)
{
	KeySym groups[][4] = {
		{ opener, opener, XK_F3, XK_F3 },
		{ XK_F1, XK_F1, opener, opener },
		{ XK_c, XK_C, XK_j, XK_J },
		{ XK_v, XK_V, XK_k, XK_K }
	};
	XkbDeviceInfoPtr keyboard = XkbGetDeviceInfo(dpy, 0, XkbUseCoreKbd, XkbDfltXIClass, XkbDfltXIId);
	XIGrabModifiers mod = { ControlMask, 0 };
	unsigned char bits[XIMaskLen(XI_KeyPress)] = {0};
	XIEventMask mask = { 0, sizeof bits, bits };
	KeyCode codes[4];
	Window first, middle, origin;
	Geometry panel = {0};
	XkbStateRec state;
	int i, attempt;

	CHECK(keyboard, "could not identify the core keyboard for the shortcut grab");
	if (!keyboard)
		return;
	mask.deviceid = keyboard->device_spec;
	XkbFreeDeviceInfo(keyboard, XkbXI_AllDeviceFeaturesMask, True);
	XISetMask(bits, XI_KeyPress);
	for (i = 0; i < 4; i++)
		codes[i] = XKeysymToKeycode(dpy, groups[i][0]);
	for (i = 0; i < 4; i++)
		XChangeKeyboardMapping(dpy, codes[i], 4, groups[i], 1);
	first = create_client(dpy, root, "layout first", 0);
	CHECK(wait_active(dpy, root, first), "first layout client was not selected");
	middle = create_client(dpy, root, "layout second", 0);
	CHECK(wait_active(dpy, root, middle), "second layout client was not selected");
	origin = create_client(dpy, root, "layout origin", 0);
	CHECK(wait_active(dpy, root, origin), "layout origin was not selected");
	for (i = 0; i < 2; i++) {
		CHECK(XkbLockGroup(dpy, XkbUseCoreKbd, i) && XkbGetState(dpy, XkbUseCoreKbd, &state) == Success
		   && state.group == i, "could not select keyboard group %d", i);
		for (attempt = 0; attempt < 80; attempt++) {
			if (!XIGrabKeycode(dpy, mask.deviceid, codes[1 - i], root, GrabModeAsync,
			                   GrabModeAsync, False, &mask, 1, &mod))
				break;
			pause_millis(25);
		}
		CHECK(attempt < 80, "group %d reserved the inactive group's root shortcut", i);
		if (attempt < 80)
			XIUngrabKeycode(dpy, mask.deviceid, codes[1 - i], root, 1, &mod);
		switcher_key(dpy, XK_Control_L, True);
		tap_keycode(dpy, codes[1 - i]);
		CHECK(wait_for_input(dpy, origin, KeyPress, codes[1 - i], ControlMask),
		      "group %d intercepted the other group's opener", i);
		switcher_key(dpy, XK_Control_L, False);
	}
	for (i = 2; i < 5; i++) {
		switcher_key(dpy, XK_Control_L, True);
		tap_keycode(dpy, codes[1]);
		CHECK(wait_panel(dpy, root, 1, &panel), "second group's Ctrl opener did not open");
		if (i == 3)
			tap_keycode(dpy, codes[2]);
		if (i < 4) {
			tap_keycode(dpy, codes[i]);
		} else {
			switcher_key(dpy, XK_Shift_L, True);
			tap_keycode(dpy, codes[1]);
			switcher_key(dpy, XK_Shift_L, False);
			CHECK(wait_panel(dpy, root, 0, &panel), "second group's Shift+opener did not cancel");
		}
		switcher_key(dpy, XK_Control_L, False);
		CHECK(wait_panel(dpy, root, 0, &panel) && wait_active(dpy, root, i == 2 ? first : origin),
		      "second group's %s left the wrong window focused", i < 4 ? XKeysymToString(groups[i][2]) : "cancel");
	}
	mod.modifiers = ControlMask | LockMask;
	CHECK(codes[0] < codes[1] && !XIGrabKeycode(dpy, mask.deviceid, codes[0], root, GrabModeAsync,
	                                         GrabModeAsync, False, &mask, 1, &mod),
	      "could not reserve Ctrl+CapsLock on the lower opener keycode");
	groups[0][2] = opener;
	groups[0][3] = opener;
	XChangeKeyboardMapping(dpy, codes[0], 4, groups[0], 1);
	XSync(dpy, True);
	CHECK(wait_configure_ack(dpy, origin), "keyboard mapping change was not acknowledged");
	switcher_key(dpy, XK_Control_L, True);
	tap_keycode(dpy, codes[1]);
	CHECK(wait_panel(dpy, root, 1, &panel), "grab refusal on one opener keycode disabled it on another");
	switcher_key(dpy, XK_Shift_L, True);
	tap_keycode(dpy, codes[1]);
	switcher_key(dpy, XK_Shift_L, False);
	switcher_key(dpy, XK_Control_L, False);
	CHECK(wait_panel(dpy, root, 0, &panel) && active_window(dpy, root) == origin,
	      "could not cancel the second opener keycode's panel");
	XIUngrabKeycode(dpy, mask.deviceid, codes[0], root, 1, &mod);
	XkbLockGroup(dpy, XkbUseCoreKbd, 0);
	check_switcher_without_numlock(dpy, root, origin);
}

static void
queued_gap_return(Display *dpy, XineramaScreenInfo *heads, int click)
{
	XGrabServer(dpy);
	XTestFakeMotionEvent(dpy, DefaultScreen(dpy), heads[1].x_org + 1,
	                     heads[1].y_org + heads[1].height - 1, CurrentTime);
	if (click) {
		XTestFakeButtonEvent(dpy, Button2, True, CurrentTime);
		XTestFakeButtonEvent(dpy, Button2, False, CurrentTime);
	}
	XTestFakeMotionEvent(dpy, DefaultScreen(dpy), heads[0].x_org + 1,
	                     heads[0].y_org + heads[0].height - 1, CurrentTime);
	XUngrabServer(dpy);
	XFlush(dpy);
}

static void
check_hidden_mixed_scratchpad(Display *dpy, Window root, XineramaScreenInfo head)
{
	Window editor, pad, probe;
	Geometry panel = {0};
	XClassHint hint = { "spterm", "St" };
	int one_row;

	editor = create_client(dpy, root, "Scratchpad test editor", 0);
	CHECK(wait_active(dpy, root, editor), "could not focus the scratchpad test editor");
	CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "could not open the one-row panel");
	one_row = geometry(dpy, switcher_panel(dpy, root, &panel)).h;
	switcher_key(dpy, XK_Control_L, False);
	CHECK(wait_panel(dpy, root, 0, &panel), "one-row panel did not close");

	pad = XCreateSimpleWindow(dpy, root, 40, 40, 180, 120, 0, 0, 0);
	XStoreName(dpy, pad, "Terminal scratchpad");
	XSetClassHint(dpy, pad, &hint);
	XSelectInput(dpy, pad, KeyPressMask);
	XMapWindow(dpy, pad);
	CHECK(wait_visible(dpy, pad, head, 0) && clientinfo_is(dpy, pad, ScratchTerm, head.screen_number),
	      "terminal scratchpad did not start hidden with only its scratchpad tag");
	CHECK(open_switcher(dpy, root, XK_Control_L, &panel)
	   && geometry(dpy, switcher_panel(dpy, root, &panel)).h == one_row,
	      "hidden scratchpad-only window added a switcher row");
	switcher_key(dpy, XK_Control_L, False);
	CHECK(wait_panel(dpy, root, 0, &panel), "scratchpad-only panel did not close");

	key(dpy, XK_Return, XK_Shift_L);
	CHECK(wait_active(dpy, root, pad), "terminal scratchpad did not show");
	switcher_key(dpy, XK_Control_L, True);
	key(dpy, XK_2, XK_Shift_L);
	key(dpy, XK_3, XK_Shift_L);
	switcher_key(dpy, XK_Control_L, False);
	key(dpy, XK_Return, XK_Shift_L);
	CHECK(wait_active(dpy, root, editor) && wait_visible(dpy, pad, head, 0)
	   && clientinfo_is(dpy, pad, ScratchTerm | 6, head.screen_number),
	      "terminal scratchpad did not hide with its ordinary tags intact");

	CHECK(open_switcher(dpy, root, XK_Control_L, &panel)
	   && geometry(dpy, switcher_panel(dpy, root, &panel)).h == 2 * one_row,
	      "hidden mixed-tag scratchpad did not add a switcher row");
	switcher_key(dpy, XK_Control_L, False);
	switcher_tap(dpy, XK_x);
	CHECK(wait_panel(dpy, root, 0, &panel)
	   && wait_for_input(dpy, pad, KeyPress, XKeysymToKeycode(dpy, XK_x), 0)
	   && active_window(dpy, root) == pad && wait_visible(dpy, pad, head, 1),
	      "Ctrl release did not activate the hidden mixed-tag scratchpad");
	probe = create_client(dpy, root, "Scratchpad view probe", 0);
	CHECK(wait_active(dpy, root, probe) && clientinfo_is(dpy, probe, 2, head.screen_number),
	      "mixed-tag scratchpad activation did not reveal only tag 2");
	XDestroyWindow(dpy, probe);
	key(dpy, XK_1, 0);
	CHECK(wait_active(dpy, root, editor), "scratchpad test did not restore tag 1");
	XDestroyWindow(dpy, pad);
	XDestroyWindow(dpy, editor);
	CHECK(wait_active(dpy, root, None), "scratchpad test did not leave an empty monitor");
}

/* Ctrl+other must reach the application, and dwm must not hold its root grab. */
static void
check_unreserved(Display *dpy, Window root, Window origin, KeySym sym, unsigned int mods)
{
	XkbDeviceInfoPtr keyboard = XkbGetDeviceInfo(dpy, 0, XkbUseCoreKbd, XkbDfltXIClass, XkbDfltXIId);
	XIGrabModifiers mod = { mods, 0 };
	unsigned char bits[XIMaskLen(XI_KeyPress)] = {0};
	XIEventMask mask = { 0, sizeof bits, bits };
	KeyCode code = XKeysymToKeycode(dpy, sym);
	Geometry panel = {0};

	CHECK(keyboard && code, "could not prepare the %s check", XKeysymToString(sym));
	if (!keyboard || !code)
		return;
	mask.deviceid = keyboard->device_spec;
	XkbFreeDeviceInfo(keyboard, XkbXI_AllDeviceFeaturesMask, True);
	XISetMask(bits, XI_KeyPress);
	CHECK(!XIGrabKeycode(dpy, mask.deviceid, code, root, GrabModeAsync, GrabModeAsync, False, &mask, 1, &mod),
	      "dwm reserved %s with modifiers %#x", XKeysymToString(sym), mods);
	XIUngrabKeycode(dpy, mask.deviceid, code, root, 1, &mod);
	XSync(dpy, True);
	switcher_key(dpy, XK_Control_L, True);
	if (mods & ShiftMask)
		switcher_key(dpy, XK_Shift_L, True);
	tap_keycode(dpy, code);
	if (mods & ShiftMask)
		switcher_key(dpy, XK_Shift_L, False);
	switcher_key(dpy, XK_Control_L, False);
	CHECK(wait_for_input(dpy, origin, KeyPress, code, ControlMask) && !switcher_panel(dpy, root, &panel)
	   && active_window(dpy, root) == origin,
	      "%s with modifiers %#x did not reach the focused application", XKeysymToString(sym), mods);
}

static void
check_switcher_step(Display *dpy, Window root)
{
	const int rows[] = { 2, 0, 1, 2 };
	KeyCode code = XKeysymToKeycode(dpy, opener);
	XKeyboardControl repeat = { .key = code, .auto_repeat_mode = AutoRepeatModeOn };
	Geometry panel = {0};
	Window target, previous, origin, panelwin, focused;
	int height, row, revert;

	target = switcher_client(dpy, root, "Step target", 1, 0);
	CHECK(wait_active(dpy, root, target), "could not focus the step target");
	previous = switcher_client(dpy, root, "Step previous", 1, 0);
	CHECK(wait_active(dpy, root, previous), "could not focus the previous step window");
	origin = switcher_client(dpy, root, "Step origin", 1, 0);
	CHECK(wait_active(dpy, root, origin), "could not focus the step origin");
	XSelectInput(dpy, origin, KeyPressMask | StructureNotifyMask);
	check_unreserved(dpy, root, origin, opener == XK_Escape ? XK_Tab : XK_Escape, ControlMask);
	check_unreserved(dpy, root, origin, opener, ControlMask | ShiftMask);
	XAutoRepeatOn(dpy);
	XChangeKeyboardControl(dpy, KBKey | KBAutoRepeatMode, &repeat);
	CHECK(XkbSetAutoRepeatRate(dpy, XkbUseCoreKbd, 150, 1000), "could not set the opener repeat rate");
	switcher_key(dpy, opener, True);
	CHECK(wait_for_input(dpy, origin, KeyPress, code, 0) && wait_for_input(dpy, origin, KeyPress, code, 0),
	      "holding the opener did not generate a repeat at the focused client");
	switcher_key(dpy, opener, False);
	CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "the Ctrl opener did not open the panel for stepping");
	panelwin = switcher_panel(dpy, root, &panel);
	height = panelwin ? geometry(dpy, panelwin).h : 0;
	row = height / 3;
	CHECK(height == 3 * row && wait_highlight(dpy, panelwin, height, row, 1)
	   && active_window(dpy, root) == origin, "the opener did not highlight the previous window while retaining focus");
	for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
		if (i == 1) {
			switcher_key(dpy, opener, True);
			CHECK(wait_highlight(dpy, panelwin, height, row, rows[i]), "held opener did not wrap to the first row");
			pause_millis(350);
			switcher_key(dpy, opener, False);
			CHECK(wait_opener_release_handled(dpy, origin), "dwm did not process the held opener release");
		} else
			switcher_tap(dpy, opener);
		CHECK(wait_highlight(dpy, panelwin, height, row, rows[i]),
		      "opener press %zu did not advance once to row %d", i + 2, rows[i]);
		XGetInputFocus(dpy, &focused, &revert);
		CHECK(focused == origin && active_window(dpy, root) == origin,
		      "opener press %zu changed focus with Ctrl held", i + 2);
	}
	switcher_key(dpy, XK_Control_L, False);
	CHECK(wait_panel(dpy, root, 0, &panel) && wait_active(dpy, root, target),
	      "Ctrl release did not activate the window reached by the opener");

	/* Cancelling after a step must neither commit the highlight nor change focus. */
	for (int escape = 0; escape < 1 + (opener != XK_Escape); escape++) {
		CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "could not open the panel to cancel");
		switcher_tap(dpy, opener);
		if (escape)
			switcher_tap(dpy, XK_Escape);
		else
			cancel_switcher(dpy);
		CHECK(wait_panel(dpy, root, 0, &panel), "%s did not cancel the panel with Ctrl held",
		      escape ? "Escape" : "Shift+opener");
		switcher_key(dpy, XK_Control_L, False);
		switcher_tap(dpy, XK_x);
		CHECK(wait_for_input(dpy, target, KeyPress, XKeysymToKeycode(dpy, XK_x), 0)
		   && active_window(dpy, root) == target,
		      "Ctrl release after %s committed a row", escape ? "Escape" : "Shift+opener");
	}
	XDestroyWindow(dpy, origin);
	XDestroyWindow(dpy, previous);
	XDestroyWindow(dpy, target);
	CHECK(wait_active(dpy, root, None), "opener navigation did not leave an empty monitor");
}

/* A keyboard beside the XTEST device on the core keyboard, like a physical one. */
static int
core_slave_keyboard(Display *dpy, XDevice *keyboard)
{
	XIDeviceInfo *devices;
	int i, n, pointer, master = 0, major = 2, minor = 0;

	if (XIQueryVersion(dpy, &major, &minor) != Success || !XIGetClientPointer(dpy, None, &pointer))
		return 0;
	devices = XIQueryDevice(dpy, XIAllDevices, &n);
	for (i = 0; devices && i < n; i++)
		if (devices[i].use == XIMasterKeyboard && devices[i].attachment == pointer)
			master = devices[i].deviceid;
	for (i = 0; devices && i < n && !keyboard->device_id; i++)
		if (devices[i].use == XISlaveKeyboard && devices[i].attachment == master
		&& !strstr(devices[i].name, "XTEST"))
			keyboard->device_id = devices[i].deviceid;
	if (devices)
		XIFreeDeviceInfo(devices);
	return keyboard->device_id != 0;
}

static void
slave_key(Display *dpy, XDevice *keyboard, KeySym sym, Bool down)
{
	XTestFakeDeviceKeyEvent(dpy, keyboard, XKeysymToKeycode(dpy, sym), down, NULL, 0, CurrentTime);
	XFlush(dpy);
}

static int
remap_keycode(Display *dpy, KeyCode code, KeySym *syms, int per, Window ack)
{
	XChangeKeyboardMapping(dpy, code, per, syms, 1);
	XSync(dpy, False);
	return wait_configure_ack(dpy, ack);
}

/* Moves the committed row to the front of the recent-first stack. */
static void
promote_row(Window *stack, int row)
{
	Window win = stack[row];

	memmove(stack + 1, stack, row * sizeof *stack);
	stack[0] = win;
}

/*
 * xcape sends the opener from the XTEST keyboard while Ctrl and Caps Lock come
 * from the physical one. The server reports each switch between the two as a
 * keyboard MappingNotify, which must not end navigation; nor must a real
 * keymap change, even one that unmaps the opener.
 */
static void
check_switcher_slave_switch(Display *dpy, Window root)
{
	const int cycle[] = { 2, 0, 1, 2 };
	XDevice physical = {0};
	KeyCode code = XKeysymToKeycode(dpy, opener), spare = XKeysymToKeycode(dpy, XK_F12);
	KeyCode x = XKeysymToKeycode(dpy, XK_x);
	KeySym *openersyms = NULL, *sparesyms = NULL, *changed = NULL;
	Window stack[3] = {0}, panelwin = None, focused;
	Geometry panel = {0};
	int i, per = 0, height = 0, row = 0, revert;

	if (!core_slave_keyboard(dpy, &physical) || !code || !spare || !x
	|| !(openersyms = XGetKeyboardMapping(dpy, code, 1, &per))
	|| !(sparesyms = XGetKeyboardMapping(dpy, spare, 1, &per))
	|| !(changed = calloc(per, sizeof *changed))) {
		CHECK(0, "slave switch regression requires a non-XTEST core keyboard and F12");
		goto out;
	}
	stack[2] = switcher_client(dpy, root, "Slave target", 1, 0);
	stack[1] = switcher_client(dpy, root, "Slave previous", 1, 0);
	stack[0] = switcher_client(dpy, root, "Slave origin", 1, 0);
	for (i = 0; i < 3; i++)
		XSelectInput(dpy, stack[i], KeyPressMask | StructureNotifyMask);
	CHECK(wait_active(dpy, root, stack[0]), "could not focus the slave switch origin");

	/* One tap: the Ctrl release switches back to the physical keyboard. */
	slave_key(dpy, &physical, XK_Control_L, True);
	switcher_tap(dpy, opener);
	if (wait_panel(dpy, root, 1, &panel) && (panelwin = switcher_panel(dpy, root, &panel)))
		height = geometry(dpy, panelwin).h;
	row = height / 3;
	CHECK(height == 3 * row && wait_highlight(dpy, panelwin, height, row, 1),
	      "the XTEST opener did not open the panel with physical Ctrl held");
	slave_key(dpy, &physical, XK_Control_L, False);
	CHECK(wait_panel(dpy, root, 0, &panel) && wait_active(dpy, root, stack[1]),
	      "physical Ctrl release after one XTEST opener did not activate the previous window");
	promote_row(stack, 1);
	switcher_tap(dpy, XK_x);
	CHECK(wait_for_input(dpy, stack[0], KeyPress, x, 0), "typing after the one-tap commit was lost");

	/* Caps Lock as Super_L, then xcape's Escape: every tap switches slaves twice. */
	slave_key(dpy, &physical, XK_Control_L, True);
	switcher_tap(dpy, opener);
	CHECK(wait_panel(dpy, root, 1, &panel) && (panelwin = switcher_panel(dpy, root, &panel))
	   && wait_highlight(dpy, panelwin, height, row, 1), "the cycle did not start on the previous window");
	for (i = 0; i < (int)(sizeof cycle / sizeof *cycle); i++) {
		slave_key(dpy, &physical, XK_Super_L, True);
		slave_key(dpy, &physical, XK_Super_L, False);
		switcher_tap(dpy, opener);
		CHECK(wait_highlight(dpy, panelwin, height, row, cycle[i]),
		      "XTEST opener %d after a physical key did not advance once to row %d", i + 2, cycle[i]);
		XGetInputFocus(dpy, &focused, &revert);
		CHECK(focused == stack[0], "XTEST opener %d changed focus with physical Ctrl held", i + 2);
	}
	slave_key(dpy, &physical, XK_Control_L, False);
	CHECK(wait_panel(dpy, root, 0, &panel) && wait_active(dpy, root, stack[2]),
	      "physical Ctrl release did not activate the row reached by the XTEST opener");
	promote_row(stack, 2);
	switcher_tap(dpy, XK_x);
	CHECK(wait_for_input(dpy, stack[0], KeyPress, x, 0), "typing after the cycled commit was lost");

	/* A real keymap change keeps navigating and still refreshes the grabs. */
	slave_key(dpy, &physical, XK_Control_L, True);
	switcher_tap(dpy, opener);
	CHECK(wait_panel(dpy, root, 1, &panel) && (panelwin = switcher_panel(dpy, root, &panel))
	   && wait_highlight(dpy, panelwin, height, row, 1), "could not open the panel before a keymap change");
	changed[0] = XK_F11;
	CHECK(remap_keycode(dpy, spare, changed, per, stack[0]) && remap_keycode(dpy, spare, sparesyms, per, stack[0])
	   && switcher_panel(dpy, root, &panel), "an unrelated keymap change closed the panel");
	switcher_tap(dpy, opener);
	CHECK(wait_highlight(dpy, panelwin, height, row, 2), "the opener did not advance after a keymap change");
	/* Unmapping the opener must not strand the panel: Ctrl release still ends it. */
	changed[0] = XK_F13;
	CHECK(remap_keycode(dpy, code, changed, per, stack[0]) && switcher_panel(dpy, root, &panel),
	      "unmapping the opener closed the panel before Ctrl release");
	slave_key(dpy, &physical, XK_Control_L, False);
	CHECK(wait_panel(dpy, root, 0, &panel) && wait_active(dpy, root, stack[2]),
	      "Ctrl release with the opener unmapped did not activate the highlighted row");
	promote_row(stack, 2);
	CHECK(remap_keycode(dpy, code, openersyms, per, stack[0]), "could not restore the opener mapping");
	switcher_tap(dpy, XK_x);
	CHECK(wait_for_input(dpy, stack[0], KeyPress, x, 0), "typing after the keymap commit was lost");

	/* Cancelling still keeps focus and typing at the original window. */
	for (i = 0; i < 1 + (opener != XK_Escape); i++) {
		slave_key(dpy, &physical, XK_Control_L, True);
		switcher_tap(dpy, opener);
		CHECK(wait_panel(dpy, root, 1, &panel), "the restored opener did not reopen the panel");
		slave_key(dpy, &physical, XK_Super_L, True);
		slave_key(dpy, &physical, XK_Super_L, False);
		if (i) {
			switcher_tap(dpy, XK_Escape);
		} else {
			slave_key(dpy, &physical, XK_Shift_L, True);
			switcher_tap(dpy, opener);
			slave_key(dpy, &physical, XK_Shift_L, False);
		}
		CHECK(wait_panel(dpy, root, 0, &panel), "%s did not cancel across a slave switch",
		      i ? "Escape" : "Shift+opener");
		slave_key(dpy, &physical, XK_Control_L, False);
		switcher_tap(dpy, XK_x);
		CHECK(wait_for_input(dpy, stack[0], KeyPress, x, 0) && active_window(dpy, root) == stack[0],
		      "Ctrl release after %s across a slave switch committed a row", i ? "Escape" : "Shift+opener");
	}
	for (i = 0; i < 3; i++)
		XDestroyWindow(dpy, stack[i]);
	CHECK(wait_active(dpy, root, None), "slave switch checks did not leave an empty monitor");
out:
	if (physical.device_id) {
		slave_key(dpy, &physical, XK_Control_L, False);
		if (code && openersyms)
			XChangeKeyboardMapping(dpy, code, per, openersyms, 1);
	}
	if (openersyms)
		XFree(openersyms);
	if (sparesyms)
		XFree(sparesyms);
	free(changed);
	XSync(dpy, False);
}

static void
test_switcher(Display *dpy, Window root)
{
	XineramaScreenInfo *heads;
	Window a1, a2, a4, a12, a23, b1, b1other, b2;
	Window pads[2][2];
	Geometry panel = {0}, b1geom, b1othergeom, a23geom, a2geom, b2geom, b2othergeom;
	int i, n = 0, separate, height_a, height_b, row;
	const int navrows[] = { 0, 4, 0, 1, 2 };
	Window panelwin, child, popup, unrelated;
	XSetWindowAttributes popupattrs = { .override_redirect = True };
	XEvent event = {0};
	XkbStateRec state;

	heads = XineramaQueryScreens(dpy, &n);
	CHECK(heads && n == 2 && (heads[0].width != heads[1].width || heads[0].height != heads[1].height
	      || heads[0].x_org != heads[1].x_org || heads[0].y_org != heads[1].y_org),
	      "switcher regression requires two distinct Xinerama heads");
	if (!heads || n != 2) {
		if (heads)
			XFree(heads);
		return;
	}
	separate = heads[0].x_org + heads[0].width <= heads[1].x_org;
	check_keyboard_scope(dpy, root);
	switcher_key(dpy, XK_Control_L, True);
	switcher_key(dpy, opener, True);
	XSync(dpy, False);
	for (i = 0; i < 80; i++) {
		if (XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess)
			break;
		pause_millis(25);
	}
	CHECK(i < 80, "empty monitor retained the keyboard grab");
	CHECK(!switcher_panel(dpy, root, &panel), "empty monitor opened a panel");
	if (i < 80)
		XUngrabKeyboard(dpy, CurrentTime);
	switcher_key(dpy, opener, False);
	switcher_key(dpy, XK_Control_L, False);
	check_hidden_mixed_scratchpad(dpy, root, heads[0]);
	check_switcher_step(dpy, root);
	check_switcher_slave_switch(dpy, root);
	/* Start on A. Then visit B to give it an independent layout/selection. */
	a23 = switcher_client(dpy, root, "Calendar - tags 2 + 3", 6, 0);
	CHECK(wait_visible(dpy, a23, heads[0], 0), "sole eligible client was not hidden");
	popup = XCreateWindow(dpy, root, 0, 0, 8, 8, 0, CopyFromParent, InputOutput,
	                      CopyFromParent, CWOverrideRedirect, &popupattrs);
	XMapWindow(dpy, popup);
	for (i = 0; i < 2; i++) {
		Window origin = i ? popup : root;
		XSetInputFocus(dpy, origin, RevertToPointerRoot, CurrentTime);
		CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "could not open from %#lx", origin);
		XSetInputFocus(dpy, i ? root : popup, RevertToPointerRoot, CurrentTime);
		XSetInputFocus(dpy, origin, RevertToPointerRoot, CurrentTime);
		CHECK(wait_panel(dpy, root, 0, &panel), "focus round trip from %#lx left panel open", origin);
		switcher_key(dpy, XK_Control_L, False);
		CHECK(wait_visible(dpy, a23, heads[0], 0), "focus round trip revealed a hidden client");
	}
	XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
	XDestroyWindow(dpy, popup);
	CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "sole client did not open a panel");
	row = geometry(dpy, switcher_panel(dpy, root, &panel)).h;
	switcher_key(dpy, XK_Control_L, False);
	CHECK(wait_active(dpy, root, a23), "sole hidden client was not activated");
	check_floating_switcher(dpy, root, a23, heads[0]);
	a2 = switcher_client(dpy, root, "Inbox - tag 2", 2, 0);
	CHECK(wait_active(dpy, root, a2), "second visible row was not selected");
	CHECK(XkbLatchModifiers(dpy, XkbUseCoreKbd, ControlMask, ControlMask)
	   && XkbGetState(dpy, XkbUseCoreKbd, &state) == Success
	   && (state.latched_mods & ControlMask) && !(state.base_mods & ControlMask),
	      "could not latch Ctrl with neither Ctrl key held");
	switcher_tap(dpy, opener);
	switcher_tap(dpy, XK_x);
	CHECK(wait_for_input(dpy, a2, KeyPress, XKeysymToKeycode(dpy, XK_x), 0)
	   && active_window(dpy, root) == a2 && !switcher_panel(dpy, root, &panel),
	      "latched Ctrl activated the other row or kept the keyboard");
	key(dpy, XK_u, XK_Shift_L);
	record_layout(dpy, a23, a2, &a23geom, &a2geom, 1);
	key(dpy, XK_1, 0);
	CHECK(wait_visible(dpy, a23, heads[0], 0), "could not restore fixture view");
	for (i = 0; i < 2; i++) {
		pads[i][0] = switcher_client(dpy, root, "Terminal scratchpad", ScratchTerm, i);
		pads[i][1] = switcher_client(dpy, root, "Calculator scratchpad", ScratchCalc, i);
		CHECK(wait_visible(dpy, pads[i][0], heads[i], 0) && wait_visible(dpy, pads[i][1], heads[i], 0),
		      "scratchpad witnesses did not start hidden on monitor %d", i);
	}
	a12 = switcher_client(dpy, root, "Notes - tags 1 + 2", 3, 0);
	a1 = switcher_client(dpy, root, "Project plan - tag 1", 1, 0);
	b2 = switcher_client(dpy, root, "Reference - tag 2", 2, 1);
	b1other = switcher_client(dpy, root, "Chat - tags 1 + 2", 3, 1);
	CHECK(wait_active(dpy, root, a1) && wait_visible(dpy, b1other, heads[1], 1),
	      "switcher fixture was not placed on two monitors");
	key(dpy, XK_l, 0);
	CHECK(wait_active(dpy, root, b1other), "could not select monitor B");
	key(dpy, XK_2, 0);
	CHECK(wait_visible(dpy, b2, heads[1], 1), "could not show B's tiled tag");
	record_layout(dpy, b2, b1other, &b2geom, &b2othergeom, 0);
	key(dpy, XK_1, 0);
	CHECK(wait_visible(dpy, b2, heads[1], 0), "could not leave B's tiled tag");
	b1 = switcher_client(dpy, root, "Editor - tag 1", 1, 1);
	CHECK(wait_active(dpy, root, b1), "could not focus editor on B");
	key(dpy, XK_u, XK_Shift_L);
	record_layout(dpy, b1, b1other, &b1geom, &b1othergeom, 1);
	CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "could not measure B's ordinary rows");
	height_b = geometry(dpy, switcher_panel(dpy, root, &panel)).h;
	cancel_switcher(dpy);
	switcher_key(dpy, XK_Control_L, False);
	key(dpy, XK_apostrophe, 0);
	CHECK(wait_active(dpy, root, pads[1][1]), "could not show B's calculator");
	queued_switch(dpy, XK_Control_L, NoSymbol, 0, b1);
	key(dpy, XK_h, 0);
	CHECK(wait_active(dpy, root, a1), "could not return to monitor A");

	/* Just 1 + 4 is viewed; committing a visible row must not switch to just 1. */
	a4 = switcher_client(dpy, root, "Archive - tag 4", 8, 0);
	CHECK(wait_visible(dpy, a4, heads[0], 0), "tag 4 fixture was not hidden");
	key(dpy, XK_4, XK_Control_L);
	key(dpy, XK_k, 0); /* focus a4 so a1 is the most recent other window */
	CHECK(wait_active(dpy, root, a4), "could not set up a multi-tag view on A");
	CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "the Ctrl opener did not open the switcher");
	if (!panel.w)
		goto out;
	CHECK(panel_on(panel, heads[0]), "panel was not centered inside monitor A");
	panelwin = switcher_panel(dpy, root, &panel);
	height_a = geometry(dpy, panelwin).h;
	CHECK(row > 0 && height_a == 5 * row && wait_highlight(dpy, panelwin, height_a, row, 1),
	      "expected five rows of height %d with the second highlighted", row);
	CHECK(active_window(dpy, root) == a4 && wait_visible(dpy, a23, heads[0], 0),
	      "opening the switcher changed focus or view");
	switcher_key(dpy, XK_Control_R, True);
	switcher_key(dpy, XK_Control_L, False);
	for (i = 0; i < 5; i++) {
		switcher_tap(dpy, i < 2 ? XK_k : XK_j);
		CHECK(wait_highlight(dpy, panelwin, height_a, row, navrows[i]) && active_window(dpy, root) == a4,
		      "navigation step %d did not highlight row %d without focusing it", i, navrows[i]);
	}
	CHECK(wait_panel(dpy, root, 1, &panel) && active_window(dpy, root) == a4,
	      "releasing one of two Ctrl keys committed the switcher");
	switcher_key(dpy, XK_Control_R, False);
	CHECK(wait_panel(dpy, root, 0, &panel) && wait_active(dpy, root, a12),
	      "releasing the final Ctrl did not commit the highlighted window");
	CHECK(wait_visible(dpy, a4, heads[0], 1) && wait_visible(dpy, a23, heads[0], 0),
	      "committing an already visible window replaced the current view");

	check_view(dpy, root, a12, 9, heads[0], pads[0]);
	queued_switch(dpy, XK_Control_L, NoSymbol, 0, a4);
	CHECK(wait_active(dpy, root, a4), "queued Ctrl opener did not activate the previous window");
	queued_switch(dpy, XK_Control_R, XK_k, 2, a23);
	CHECK(wait_panel(dpy, root, 0, &panel) && wait_active(dpy, root, a23),
	      "reverse wrap did not reach the one row for the multi-tag window");
	CHECK(wait_visible(dpy, a2, heads[0], 1) && wait_visible(dpy, a4, heads[0], 0)
	   && wait_visible(dpy, a1, heads[0], 0), "hidden multi-tag target did not reveal just tag 2");
	CHECK(wait_geometry(dpy, a23, a23geom) && wait_geometry(dpy, a2, a2geom),
	      "hidden activation did not restore A's monocle layout");
	queued_switch(dpy, XK_Control_L, XK_j, 5, a4);
	CHECK(wait_active(dpy, root, a4), "hidden activation promoted an unchosen window");
	queued_switch(dpy, XK_Control_L, NoSymbol, 0, a23);
	CHECK(wait_active(dpy, root, a23), "could not return to the hidden target");
	check_view(dpy, root, a23, 2, heads[0], pads[0]);
	key(dpy, XK_Return, XK_Shift_L);
	CHECK(wait_active(dpy, root, pads[0][0]), "could not show A's terminal scratchpad");
	queued_switch(dpy, XK_Control_L, NoSymbol, 0, a23);
	check_view(dpy, root, a23, 2 | ScratchTerm, heads[0], pads[0]);
	CHECK(clientinfo_is(dpy, a23, 6, 0) && clientinfo_is(dpy, a12, 3, 0),
	      "activation retagged or migrated a monitor A client");
	CHECK(same_geometry(geometry(dpy, b1), b1geom)
	   && same_geometry(geometry(dpy, b1other), b1othergeom)
	   && wait_visible(dpy, b2, heads[1], 0), "monitor A activation changed monitor B's view or layout");

	/* Count rows by their uniform height, including multi-tag windows only once. */
	key(dpy, XK_l, 0);
	CHECK(wait_active(dpy, root, b1), "monitor B did not remember its selection");
	check_view(dpy, root, b1, 1 | ScratchCalc, heads[1], pads[1]);
	CHECK(open_switcher(dpy, root, XK_Control_L, &panel) && panel_on(panel, heads[1]),
	      "reopened panel was not centered inside monitor B");
	CHECK(row > 0 && height_b == 3 * row, "expected three rows on B with both scratchpads hidden");
	CHECK(geometry(dpy, switcher_panel(dpy, root, &panel)).h == 4 * row,
	      "shown calculator did not add one row on B");
	switcher_tap(dpy, XK_j);
	switcher_tap(dpy, XK_j);
	switcher_key(dpy, XK_Control_L, False);
	CHECK(wait_panel(dpy, root, 0, &panel) && wait_active(dpy, root, b2)
	   && wait_visible(dpy, b1, heads[1], 0) && clientinfo_is(dpy, b2, 2, 1),
	      "monitor B activation did not reveal its own hidden client");
	CHECK(wait_geometry(dpy, b2, b2geom) && wait_geometry(dpy, b1other, b2othergeom),
	      "hidden activation did not restore B's tiled layout");
	check_view(dpy, root, b2, 2, heads[1], pads[1]);
	CHECK(same_geometry(geometry(dpy, a23), a23geom) && wait_visible(dpy, a1, heads[0], 0),
	      "monitor B activation changed monitor A's view or layout");
	key(dpy, XK_h, 0);
	CHECK(wait_active(dpy, root, a23), "monitor A did not remember its selection");
	check_view(dpy, root, a23, 2 | ScratchTerm, heads[0], pads[0]);
	key(dpy, XK_Return, XK_Shift_L);
	CHECK(wait_visible(dpy, pads[0][0], heads[0], 0), "could not hide A's scratchpad");

	/* Leave the cancelling opener held long enough for auto-repeat to try to reopen. */
	CHECK(XkbSetAutoRepeatRate(dpy, XkbUseCoreKbd, 150, 50), "could not set the test repeat rate");
	CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "could not open for cancel");
	switcher_tap(dpy, XK_j);
	switcher_key(dpy, XK_Shift_L, True);
	switcher_key(dpy, opener, True);
	CHECK(wait_panel(dpy, root, 0, &panel), "Shift+opener did not dismiss the panel");
	switcher_key(dpy, XK_Shift_L, False);
	pause_millis(350);
	CHECK(!switcher_panel(dpy, root, &panel), "held opener reopened the switcher after cancel");
	switcher_key(dpy, opener, False);
	XSync(dpy, True);
	switcher_tap(dpy, XK_j);
	CHECK(wait_for_input(dpy, a23, KeyPress, XKeysymToKeycode(dpy, XK_j), ControlMask),
	      "the client did not receive Ctrl+j immediately after cancel");
	switcher_tap(dpy, XK_k);
	CHECK(wait_for_input(dpy, a23, KeyPress, XKeysymToKeycode(dpy, XK_k), ControlMask),
	      "the client did not receive Ctrl+k immediately after cancel");
	switcher_key(dpy, XK_Control_L, False);
	CHECK(active_window(dpy, root) == a23 && same_geometry(geometry(dpy, a23), a23geom),
	      "cancelling the switcher changed view or focus");

	CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "could not open for an external focus change");
	XSetInputFocus(dpy, b2, RevertToPointerRoot, CurrentTime);
	CHECK(wait_panel(dpy, root, 0, &panel), "external focus change left the switcher armed");
	switcher_key(dpy, XK_Control_L, False);
	CHECK(active_window(dpy, root) == a23, "release after an external focus change activated a row");

	child = XCreateSimpleWindow(dpy, a23, 0, 0, 8, 8, 0, 0, 0);
	popup = XCreateWindow(dpy, root, 0, 0, 8, 8, 0, CopyFromParent, InputOutput,
	                      CopyFromParent, CWOverrideRedirect, &popupattrs);
	XMapWindow(dpy, child);
	XMapWindow(dpy, popup);
	XSetInputFocus(dpy, child, RevertToParent, CurrentTime);
	CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "could not open with focus on a child");
	XSetInputFocus(dpy, popup, RevertToParent, CurrentTime);
	XSetInputFocus(dpy, child, RevertToParent, CurrentTime);
	CHECK(wait_panel(dpy, root, 0, &panel), "child-to-popup focus round trip left the panel armed");
	switcher_key(dpy, XK_Control_L, False);
	XSetInputFocus(dpy, a23, RevertToPointerRoot, CurrentTime);
	XDestroyWindow(dpy, child);
	XDestroyWindow(dpy, popup);

	CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "could not open for unrelated updates");
	event.xconfigure = (XConfigureEvent){ .type = ConfigureNotify, .event = root, .window = root,
		.width = DisplayWidth(dpy, DefaultScreen(dpy)), .height = DisplayHeight(dpy, DefaultScreen(dpy)) };
	XSendEvent(dpy, root, False, StructureNotifyMask, &event);
	unrelated = switcher_client(dpy, root, "unlisted B client", 1, 1);
	CHECK(wait_visible(dpy, unrelated, heads[1], 0), "B client was not managed");
	XDestroyWindow(dpy, unrelated);
	switcher_key(dpy, XK_Control_L, False);
	CHECK(wait_active(dpy, root, a4), "unrelated configure, manage or unmanage dismissed A's panel");
	queued_switch(dpy, XK_Control_L, NoSymbol, 0, a23);
	CHECK(wait_active(dpy, root, a23), "could not restore A's selection");
	XSelectInput(dpy, a23, KeyPressMask | StructureNotifyMask);
	CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "could not open for target removal");
	XSync(dpy, True);
	CHECK(wait_opener_release_handled(dpy, a23), "opener release was not handled before target removal");
	XGrabServer(dpy);
	XDestroyWindow(dpy, a12);
	switcher_tap(dpy, opener);
	switcher_key(dpy, XK_Control_L, False);
	XUngrabServer(dpy);
	CHECK(wait_opener_release_handled(dpy, a23), "queued opener was not handled after target removal");
	CHECK(wait_panel(dpy, root, 0, &panel), "queued opener reopened the switcher after target removal");
	switcher_tap(dpy, XK_x);
	CHECK(wait_for_input(dpy, a23, KeyPress, XKeysymToKeycode(dpy, XK_x), 0)
	   && active_window(dpy, root) == a23 && wait_visible(dpy, a1, heads[0], 0),
	      "release after target removal stole typing or switched to another row");

	if (XGrabKeyboard(dpy, a23, False, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess) {
		switcher_key(dpy, XK_Control_L, True);
		switcher_tap(dpy, opener);
		CHECK(wait_for_input(dpy, a23, KeyPress, XKeysymToKeycode(dpy, opener), ControlMask)
		   && !switcher_panel(dpy, root, &panel), "switcher interrupted an application keyboard grab");
		switcher_key(dpy, XK_Control_L, False);
		XUngrabKeyboard(dpy, CurrentTime);
		XSync(dpy, False);
	} else
		CHECK(0, "application could not grab the keyboard after switcher cancellation");

	key(dpy, XK_f, 0);
	CHECK(wait_geometry(dpy, a23, (Geometry){heads[0].x_org, heads[0].y_org,
	                                       heads[0].width, heads[0].height}),
	      "could not make the selected client fullscreen");
	switcher_key(dpy, XK_Control_L, True);
	switcher_tap(dpy, opener);
	pause_millis(50);
	CHECK(!switcher_panel(dpy, root, &panel), "switcher bypassed the fullscreen lock");
	switcher_key(dpy, XK_Control_L, False);
	key(dpy, XK_f, 0);
	CHECK(wait_visible(dpy, a2, heads[0], 1) && active_window(dpy, root) == a23,
	      "fullscreen switcher attempt changed view or focus");

	if (separate) {
		enum { RootMotion, RoundTrip, Closed, Pending, Held, CrossingFirst, ReleaseFirst } order;

		XSelectInput(dpy, b2, KeyPressMask | PointerMotionMask | FocusChangeMask);
		XSelectInput(dpy, a4, KeyPressMask | FocusChangeMask);
		XSelectInput(dpy, root, SubstructureNotifyMask | PointerMotionMask);
		XSelectInput(dpy, a23, KeyPressMask | PointerMotionMask | StructureNotifyMask);
		unrelated = switcher_client(dpy, root, "same-head restack witness", ScratchCalc, 0);
		CHECK(wait_visible(dpy, unrelated, heads[0], 0), "same-head restack witness was not hidden");
		XTestFakeMotionEvent(dpy, DefaultScreen(dpy), heads[0].x_org + 1,
		                     heads[0].y_org + heads[0].height - 1, CurrentTime);
		XTestFakeMotionEvent(dpy, DefaultScreen(dpy), heads[1].x_org + 1,
		                     heads[1].y_org + heads[1].height - 1, CurrentTime);
		CHECK(wait_active(dpy, root, b2), "could not seed root motion on B");
		XWarpPointer(dpy, None, a23, 0, 0, 0, 0, 20, 20);
		CHECK(wait_active(dpy, root, a23), "could not return to A by client-only motion");
		XSync(dpy, True);
		XGrabServer(dpy);
		XDestroyWindow(dpy, unrelated);
		XWarpPointer(dpy, None, b2, 0, 0, 0, 0, 20, 20);
		switcher_key(dpy, XK_Control_L, True);
		switcher_tap(dpy, opener);
		XTestFakeMotionEvent(dpy, DefaultScreen(dpy), heads[1].x_org + 1,
		                     heads[1].y_org + heads[1].height - 1, CurrentTime);
		XUngrabServer(dpy);
		CHECK(wait_configure_ack(dpy, a23) && switcher_panel(dpy, root, &panel) && panel_on(panel, heads[0])
		   && active_window(dpy, root) == a23, "same-head motion on B canceled the queued opener on A");
		cancel_switcher(dpy);
		switcher_key(dpy, XK_Control_L, False);
		CHECK(wait_panel(dpy, root, 0, &panel), "could not dismiss panel after same-head motion");

		for (order = RootMotion; order <= ReleaseFirst; order++) {
			unrelated = order == Held ? pads[0][1] : switcher_client(dpy, root, "restack witness", ScratchCalc, 0);
			XWarpPointer(dpy, None, a23, 0, 0, 0, 0, 20, 20);
			CHECK(wait_visible(dpy, unrelated, heads[0], 0) && wait_active(dpy, root, a23)
			   && wait_visible(dpy, a4, heads[0], 0), "restack did not start with target and witness hidden");
			if (order == RootMotion || order == RoundTrip) {
				if (order == RootMotion) {
					XSync(dpy, True);
					XTestFakeMotionEvent(dpy, DefaultScreen(dpy), heads[0].x_org + 1,
					                     heads[0].y_org + heads[0].height - 1, CurrentTime);
				}
				XWarpPointer(dpy, None, b2, 0, 0, 0, 0, 20, 20);
				CHECK(wait_active(dpy, root, b2), "pointer did not start on B");
				if (order == RootMotion)
					CHECK(XCheckTypedWindowEvent(dpy, root, MotionNotify, &event)
					   && XCheckTypedWindowEvent(dpy, b2, MotionNotify, &event),
					      "expected root motion on A and only client motion on entering B");
				key(dpy, XK_h, 0);
				CHECK(wait_active(dpy, root, a23), "keyboard did not select A");
			}
			if (order >= Held)
				CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "could not open before restack");
			XSync(dpy, True);
			if (order == ReleaseFirst)
				CHECK(wait_opener_release_handled(dpy, a23), "dwm did not acknowledge the post-opener configure request");
			XGrabServer(dpy);
			XDestroyWindow(dpy, unrelated);
			if (order == RoundTrip)
				XTestFakeMotionEvent(dpy, DefaultScreen(dpy), a23geom.x + 20, a23geom.y + 20, CurrentTime);
			if (order == RoundTrip || order == Pending) {
				switcher_key(dpy, XK_Control_L, True);
				switcher_tap(dpy, opener);
			}
			if (order == ReleaseFirst) {
				switcher_key(dpy, XK_Control_L, False);
				CHECK(XkbGetState(dpy, XkbUseCoreKbd, &state) == Success && !(state.base_mods & ControlMask),
				      "Ctrl release was still frozen before crossing");
			}
			if (order == RootMotion)
				XTestFakeMotionEvent(dpy, DefaultScreen(dpy), heads[1].x_org + 1,
				                     heads[1].y_org + heads[1].height - 1, CurrentTime);
			else
				/* Xephyr can clip absolute motion after a cross-head warp. */
				XWarpPointer(dpy, None, b2, 0, 0, 0, 0, 20, 20);
			if (order == CrossingFirst)
				switcher_key(dpy, XK_Control_L, False);
			XUngrabServer(dpy);
			XSync(dpy, False);
			if (order == RootMotion) {
				CHECK(wait_active(dpy, root, b2), "closed restack swallowed B root motion");
				key(dpy, XK_h, 0);
				CHECK(wait_active(dpy, root, a23), "could not restore keyboard selection to A");
				continue;
			}
			if (order == RoundTrip)
				CHECK(XCheckTypedWindowEvent(dpy, a23, MotionNotify, &event), "A client did not consume motion");
			CHECK(XCheckTypedWindowEvent(dpy, b2, MotionNotify, &event), "B client did not consume motion");
			if (order == RoundTrip || order == Pending) {
				for (i = 0; i < 80 && !XCheckTypedWindowEvent(dpy, root, MapNotify, &event); i++)
					pause_millis(25);
				CHECK(i < 80 && event.xmap.override_redirect, "pending opener did not map the panel");
			} else if (order == Closed) {
				CHECK(open_switcher(dpy, root, XK_Control_L, &panel) && panel_on(panel, heads[0]),
				      "closed restack changed the selected monitor");
				cancel_switcher(dpy);
			}
			CHECK(wait_panel(dpy, root, 0, &panel), "restack left the panel armed in order %d", order);
			if (order <= Held)
				switcher_key(dpy, XK_Control_L, False);
			if (order == ReleaseFirst) {
				CHECK(wait_visible(dpy, a4, heads[0], 1), "later crossing discarded the hidden target");
				for (i = 0; i < 80 && !XCheckTypedWindowEvent(dpy, a4, FocusIn, &event); i++)
					pause_millis(25);
				CHECK(i < 80, "release before crossing did not focus the target");
			} else {
				switcher_tap(dpy, XK_x);
				CHECK(wait_for_input(dpy, a23, KeyPress, XKeysymToKeycode(dpy, XK_x), 0)
				   && wait_visible(dpy, a4, heads[0], 0), "release after crossing activated a stale row");
			}
		}
		XWarpPointer(dpy, None, a4, 0, 0, 0, 0, 20, 20);
		CHECK(wait_active(dpy, root, a4), "could not return to the committed target");
		queued_switch(dpy, XK_Control_L, NoSymbol, 0, a23);
		CHECK(wait_active(dpy, root, a23), "could not restore A's origin");
		XWarpPointer(dpy, None, a23, 0, 0, 0, 0, 20, 20);
		XSync(dpy, True);
		CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "could not open for queued crossing");
		queued_gap_return(dpy, heads, 0);
		CHECK(wait_panel(dpy, root, 0, &panel), "queued crossing and return left the panel armed");
		for (i = 0; i < 80 && !XCheckTypedWindowEvent(dpy, b2, FocusIn, &event); i++)
			pause_millis(25);
		CHECK(i < 80 && wait_active(dpy, root, a23), "queued crossing did not visit B and return focus to A");
		switcher_key(dpy, XK_Control_L, False);
		switcher_tap(dpy, XK_x);
		CHECK(wait_for_input(dpy, a23, KeyPress, XKeysymToKeycode(dpy, XK_x), 0)
		   && active_window(dpy, root) == a23 && wait_visible(dpy, a4, heads[0], 0),
		      "release after queued crossing activated a stale row");
		queued_gap_return(dpy, heads, 1);
		for (i = 0; i < 80 && geometry(dpy, b2).y >= b2geom.y; i++)
			pause_millis(25);
		CHECK(i < 80 && same_geometry(geometry(dpy, a23), a23geom),
		      "queued middle click did not hide only B's bar");
		queued_gap_return(dpy, heads, 1);
		CHECK(wait_geometry(dpy, b2, b2geom), "second queued click did not restore B's bar");
		key(dpy, XK_l, 0);
		CHECK(wait_active(dpy, root, b2), "could not select B with pointer on A");
		CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "could not open on keyboard-selected B");
		XTestFakeRelativeMotionEvent(dpy, 1, 0, CurrentTime);
		switcher_key(dpy, XK_Control_L, False);
		CHECK(wait_active(dpy, root, b1), "same-monitor root motion dismissed B's panel");
		queued_switch(dpy, XK_Control_L, NoSymbol, 0, b2);
		CHECK(wait_active(dpy, root, b2), "could not restore B's selection");
		key(dpy, XK_h, 0);
		CHECK(wait_active(dpy, root, a23), "could not return to A before pointer crossing");
		XTestFakeMotionEvent(dpy, DefaultScreen(dpy), heads[0].x_org + 1,
		                     heads[0].y_org + heads[0].height - 1, CurrentTime);
		XTestFakeMotionEvent(dpy, DefaultScreen(dpy), heads[1].x_org + 1, heads[1].y_org + 1, CurrentTime);
		CHECK(open_switcher(dpy, root, XK_Control_L, &panel) && panel_on(panel, heads[0])
		   && active_window(dpy, root) == a23, "closed pointer motion into B's bar selected B");
		XTestFakeMotionEvent(dpy, DefaultScreen(dpy), heads[0].x_org + 1,
		                     heads[0].y_org + heads[0].height - 1, CurrentTime);
		XTestFakeMotionEvent(dpy, DefaultScreen(dpy), heads[1].x_org + 1,
		                     heads[1].y_org + heads[1].height - 1, CurrentTime);
		CHECK(wait_panel(dpy, root, 0, &panel) && wait_active(dpy, root, b2),
		      "pointer crossing did not cancel the switcher and select monitor B");
		switcher_key(dpy, XK_Control_L, False);
		CHECK(open_switcher(dpy, root, XK_Control_L, &panel) && panel_on(panel, heads[1]),
		      "reopening after pointer monitor selection did not stay on B");
		cancel_switcher(dpy);
		switcher_key(dpy, XK_Control_L, False);
		CHECK(wait_panel(dpy, root, 0, &panel), "B's panel did not close before the bar crossing test");
		XTestFakeMotionEvent(dpy, DefaultScreen(dpy), heads[1].x_org + 1, heads[1].y_org + 1, CurrentTime);
		for (i = 0; i < 2; i++) {
			CHECK(open_switcher(dpy, root, XK_Control_L, &panel) && panel_on(panel, heads[1 - i]),
			      "could not open on the monitor under the pointer's bar");
			XTestFakeMotionEvent(dpy, DefaultScreen(dpy), heads[i].x_org + 1, heads[i].y_org + 1, CurrentTime);
			CHECK(wait_panel(dpy, root, 0, &panel), "bar-to-bar crossing left the panel armed");
			switcher_key(dpy, XK_Control_L, False);
			CHECK(wait_active(dpy, root, i ? b2 : a23), "bar-to-bar crossing selected the wrong window");
		}
		puts("X11 switcher side-by-side placement and pointer crossing checked");
	}
	check_switcher_group(dpy, root);
out:
	switcher_key(dpy, XK_Control_L, False);
	XFree(heads);
}

int
main(int argc, char **argv)
{
	Display *dpy = XOpenDisplay(NULL);
	Window root;
	int event_base, error_base, major, minor;
	KeyCode supercode, qcode, jcode, acode;

	if (!dpy) {
		fprintf(stderr, "could not open the private X display\n");
		return EXIT_FAILURE;
	}
	if (!XTestQueryExtension(dpy, &event_base, &error_base, &major, &minor)) {
		fprintf(stderr, "X11 input regression checks require the XTEST server extension\n");
		XCloseDisplay(dpy);
		return EXIT_FAILURE;
	}
	root = DefaultRootWindow(dpy);
	if (argc == 3 && !strcmp(argv[1], "switcher-appearance")) {
		char *end;
		long pid = strtol(argv[2], &end, 10);

		if (pid <= 1 || *end || (pid_t)pid != pid) {
			XCloseDisplay(dpy);
			return EXIT_FAILURE;
		}
		default_xerror = XSetErrorHandler(switcher_xerror);
		test_switcher_appearance(dpy, root, (pid_t)pid);
		test_switcher_closed_reload(dpy, root, (pid_t)pid);
		XCloseDisplay(dpy);
		if (!failures)
			puts("X11 switcher appearance and live reload checks passed");
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	if (argc == 3 && !strcmp(argv[1], "switcher-key")) {
		opener = XStringToKeysym(argv[2]);
		if (opener == NoSymbol || !XKeysymToKeycode(dpy, opener)) {
			fprintf(stderr, "unknown switcher key %s\n", argv[2]);
			XCloseDisplay(dpy);
			return EXIT_FAILURE;
		}
		default_xerror = XSetErrorHandler(switcher_xerror);
		/* This check starts with dwm; wait until it manages the root window. */
		for (int i = 0; i < 80 && !wm_running(dpy, root); i++)
			pause_millis(25);
		check_switcher_step(dpy, root);
		check_switcher_slave_switch(dpy, root);
		XCloseDisplay(dpy);
		if (!failures)
			printf("X11 window switcher checks passed for a Ctrl+%s build\n", argv[2]);
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	if (argc == 2 && !strcmp(argv[1], "switcher")) {
		default_xerror = XSetErrorHandler(switcher_xerror);
		test_switcher(dpy, root);
		XCloseDisplay(dpy);
		if (!failures)
			puts("X11 monitor-local window switcher checks passed");
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	supercode = XKeysymToKeycode(dpy, XK_Super_L);
	qcode = XKeysymToKeycode(dpy, XK_q);
	jcode = XKeysymToKeycode(dpy, XK_j);
	acode = XKeysymToKeycode(dpy, XK_a);
	CHECK(supercode != 0 && qcode != 0 && jcode != 0 && acode != 0,
	      "X server keymap is missing a required test keysym");
	if (supercode && qcode && jcode && acode) {
		test_idle_close(dpy, root, supercode, qcode);
		test_click_replay(dpy, root, DefaultScreen(dpy), supercode, jcode, acode);
		test_empty_title(dpy, root);
		test_pertag_views(dpy, root);
		test_scratchpad_toggle(dpy, root);
	}
	XCloseDisplay(dpy);
	if (failures)
		return EXIT_FAILURE;
	puts("X11 input regression checks passed");
	return EXIT_SUCCESS;
}
