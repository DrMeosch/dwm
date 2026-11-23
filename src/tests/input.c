#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xproto.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xinerama.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

static int failures;
static int (*default_xerror)(Display *, XErrorEvent *);

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
	if (e->error_code == BadWindow && (e->request_code == X_GetWindowAttributes
	                               || e->request_code == X_GetGeometry))
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
	switcher_tap(dpy, XK_Tab);
	return wait_panel(dpy, root, 1, outer);
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
	switcher_tap(dpy, XK_Tab);
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
highlight_height(Display *dpy, Window panel, int height)
{
	XImage *image;
	int i, y, highlighted = 0;

	for (i = 0; panel && height > 0 && !highlighted && i < 80; i++) {
		image = XGetImage(dpy, panel, 0, 0, 1, height, AllPlanes, ZPixmap);
		if (!image)
			break;
		for (y = 1; y < height; y++)
			highlighted += XGetPixel(image, 0, y) != XGetPixel(image, 0, 0);
		XDestroyImage(image);
		if (!highlighted)
			pause_millis(25);
	}
	return highlighted;
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

static void
switcher_drags(Display *dpy, Window root, Window parent)
{
	Window dialog = XCreateSimpleWindow(dpy, root, 0, 0, 160, 100, 0, 0, 0);
	Geometry before, after, panel = {0};
	int i, button;

	XSetTransientForHint(dpy, dialog, parent);
	XMapWindow(dpy, dialog);
	CHECK(wait_active(dpy, root, dialog), "drag fixture was not focused");
	for (button = Button1; button <= Button3; button += 2) {
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
		switcher_key(dpy, XK_Control_L, True);
		switcher_tap(dpy, XK_Tab);
		XTestFakeButtonEvent(dpy, button, False, CurrentTime);
		CHECK(wait_panel(dpy, root, 1, &panel), "queued opener blocked button %d release", button);
		switcher_tap(dpy, XK_Escape);
		switcher_key(dpy, XK_Control_L, False);
		CHECK(wait_panel(dpy, root, 0, &panel), "could not dismiss panel after drag");
	}
	XDestroyWindow(dpy, dialog);
	CHECK(wait_active(dpy, root, parent), "removing drag fixture did not restore focus");
}

static void
check_keyboard_scope(Display *dpy, Window root)
{
	XIAnyHierarchyChangeInfo change = { .add = { XIAddMaster, "switcher-test", True, True } };
	XIGrabModifiers mod = { ControlMask, 0 };
	unsigned char bits[XIMaskLen(XI_KeyPress)] = {0};
	XIEventMask mask = { 0, sizeof bits, bits };
	XIDeviceInfo *devices;
	Window probe;
	int i, n, major = 2, minor = 0;

	if (XIQueryVersion(dpy, &major, &minor) != Success || XIChangeHierarchy(dpy, &change, 1) != Success) {
		CHECK(0, "could not create independent test keyboard");
		return;
	}
	probe = create_client(dpy, root, "keyboard scope probe", 0);
	CHECK(wait_active(dpy, root, probe), "keyboard scope probe was not managed");
	devices = XIQueryDevice(dpy, XIAllMasterDevices, &n);
	for (i = 0; devices && i < n; i++) {
		if (devices[i].use != XIMasterKeyboard || strcmp(devices[i].name, "switcher-test keyboard"))
			continue;
		mask.deviceid = devices[i].deviceid;
		XISetMask(bits, XI_KeyPress);
		CHECK(XIGrabKeycode(dpy, mask.deviceid, XKeysymToKeycode(dpy, XK_Tab), root, GrabModeAsync,
		                    GrabModeAsync, False, &mask, 1, &mod) == 0,
		      "dwm reserved Ctrl+Tab on an independent keyboard");
		change.remove = (XIRemoveMasterInfo){ .type = XIRemoveMaster, .deviceid = mask.deviceid,
		                                     .return_mode = XIFloating };
		XIChangeHierarchy(dpy, &change, 1);
		break;
	}
	CHECK(mask.deviceid, "independent test keyboard was not found");
	if (devices)
		XIFreeDeviceInfo(devices);
	XDestroyWindow(dpy, probe);
	CHECK(wait_active(dpy, root, None), "keyboard scope probe retained focus");
}

static void
check_switcher_group(Display *dpy, Window root)
{
	KeySym groups[][4] = {
		{ XK_Tab, XK_ISO_Left_Tab, XK_F3, XK_F3 },
		{ XK_F1, XK_F1, XK_Tab, XK_ISO_Left_Tab },
		{ XK_c, XK_C, XK_j, XK_J },
		{ XK_v, XK_V, XK_k, XK_K },
		{ XK_F2, XK_F2, XK_Escape, XK_Escape }
	};
	KeyCode codes[5];
	Window first, middle, origin;
	Geometry panel = {0};
	XkbStateRec state;
	int i;

	for (i = 0; i < 5; i++)
		codes[i] = XKeysymToKeycode(dpy, groups[i][0]);
	for (i = 0; i < 5; i++)
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
		switcher_key(dpy, XK_Control_L, True);
		tap_keycode(dpy, codes[1 - i]);
		CHECK(wait_for_input(dpy, origin, KeyPress, codes[1 - i], ControlMask),
		      "group %d intercepted the other group's opener", i);
		switcher_key(dpy, XK_Control_L, False);
	}
	for (i = 2; i < 5; i++) {
		switcher_key(dpy, XK_Control_L, True);
		tap_keycode(dpy, codes[1]);
		CHECK(wait_panel(dpy, root, 1, &panel), "second group's Ctrl+Tab did not open");
		if (i == 3)
			tap_keycode(dpy, codes[2]);
		tap_keycode(dpy, codes[i]);
		if (i == 4)
			CHECK(wait_panel(dpy, root, 0, &panel), "second group's Escape did not cancel");
		switcher_key(dpy, XK_Control_L, False);
		CHECK(wait_panel(dpy, root, 0, &panel) && wait_active(dpy, root, i == 2 ? first : origin),
		      "second group's %s left the wrong window focused", XKeysymToString(groups[i][2]));
	}
	XkbLockGroup(dpy, XkbUseCoreKbd, 0);
}

static void
test_switcher(Display *dpy, Window root)
{
	XineramaScreenInfo *heads;
	Window a1, a2, a4, a12, a23, b1, b1other, b2;
	Window pads[2][2];
	Geometry panel = {0}, b1geom, b1othergeom, a23geom, a2geom, b2geom, b2othergeom;
	int i, n = 0, separate, height_a, height_b, height_one, row;
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
	height_one = geometry(dpy, switcher_panel(dpy, root, &panel)).h;
	switcher_key(dpy, XK_Control_L, False);
	CHECK(wait_active(dpy, root, a23), "sole hidden client was not activated");
	switcher_drags(dpy, root, a23);
	a2 = switcher_client(dpy, root, "Inbox - tag 2", 2, 0);
	CHECK(wait_active(dpy, root, a2), "second visible row was not selected");
	CHECK(XkbLatchModifiers(dpy, XkbUseCoreKbd, ControlMask, ControlMask)
	   && XkbGetState(dpy, XkbUseCoreKbd, &state) == Success
	   && (state.latched_mods & ControlMask) && !(state.base_mods & ControlMask),
	      "could not latch Ctrl with neither Ctrl key held");
	switcher_tap(dpy, XK_Tab);
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
	switcher_tap(dpy, XK_Escape);
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
	CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "Ctrl+Tab did not open the switcher");
	if (!panel.w)
		goto out;
	CHECK(panel_on(panel, heads[0]), "panel was not centered inside monitor A");
	panelwin = switcher_panel(dpy, root, &panel);
	height_a = geometry(dpy, panelwin).h;
	row = highlight_height(dpy, panelwin, height_a);
	CHECK(row > 0 && height_one == row && height_a == 5 * row,
	      "expected one and five rows, got heights %d and %d (highlight %d)", height_one, height_a, row);
	CHECK(active_window(dpy, root) == a4 && wait_visible(dpy, a23, heads[0], 0),
	      "opening the switcher changed focus or view");
	switcher_key(dpy, XK_Control_R, True);
	switcher_key(dpy, XK_Control_L, False);
	switcher_tap(dpy, XK_j); /* the other visible row after a1 is a12 */
	CHECK(wait_panel(dpy, root, 1, &panel) && active_window(dpy, root) == a4,
	      "releasing one of two Ctrl keys committed the switcher");
	switcher_key(dpy, XK_Control_R, False);
	CHECK(wait_panel(dpy, root, 0, &panel) && wait_active(dpy, root, a12),
	      "releasing the final Ctrl did not commit the highlighted window");
	CHECK(wait_visible(dpy, a4, heads[0], 1) && wait_visible(dpy, a23, heads[0], 0),
	      "committing an already visible window replaced the current view");

	check_view(dpy, root, a12, 9, heads[0], pads[0]);
	queued_switch(dpy, XK_Control_L, NoSymbol, 0, a4);
	CHECK(wait_active(dpy, root, a4), "queued Ctrl+Tab did not activate the previous window");
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

	/* Leave Tab held after Escape long enough for auto-repeat to try to reopen. */
	CHECK(XkbSetAutoRepeatRate(dpy, XkbUseCoreKbd, 150, 50), "could not set the test repeat rate");
	switcher_key(dpy, XK_Control_L, True);
	switcher_key(dpy, XK_Tab, True);
	CHECK(wait_panel(dpy, root, 1, &panel), "could not open for Escape");
	switcher_tap(dpy, XK_j);
	switcher_tap(dpy, XK_Escape);
	CHECK(wait_panel(dpy, root, 0, &panel), "Escape did not dismiss the panel");
	pause_millis(350);
	CHECK(!switcher_panel(dpy, root, &panel), "held Tab reopened the switcher after Escape");
	switcher_key(dpy, XK_Tab, False);
	XSync(dpy, True);
	switcher_tap(dpy, XK_j);
	CHECK(wait_for_input(dpy, a23, KeyPress, XKeysymToKeycode(dpy, XK_j), ControlMask),
	      "the client did not receive Ctrl+j immediately after Escape");
	switcher_tap(dpy, XK_k);
	CHECK(wait_for_input(dpy, a23, KeyPress, XKeysymToKeycode(dpy, XK_k), ControlMask),
	      "the client did not receive Ctrl+k immediately after Escape");
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
	CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "could not open for target removal");
	XDestroyWindow(dpy, a12);
	CHECK(wait_panel(dpy, root, 0, &panel), "target removal left the switcher armed");
	switcher_key(dpy, XK_Control_L, False);
	CHECK(active_window(dpy, root) == a23 && wait_visible(dpy, a1, heads[0], 0),
	      "release after target removal switched to another row");

	if (XGrabKeyboard(dpy, a23, False, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess) {
		switcher_key(dpy, XK_Control_L, True);
		switcher_tap(dpy, XK_Tab);
		CHECK(wait_for_input(dpy, a23, KeyPress, XKeysymToKeycode(dpy, XK_Tab), ControlMask)
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
	switcher_tap(dpy, XK_Tab);
	pause_millis(50);
	CHECK(!switcher_panel(dpy, root, &panel), "switcher bypassed the fullscreen lock");
	switcher_key(dpy, XK_Control_L, False);
	key(dpy, XK_f, 0);
	CHECK(wait_visible(dpy, a2, heads[0], 1) && active_window(dpy, root) == a23,
	      "fullscreen switcher attempt changed view or focus");

	if (separate) {
		XTestFakeMotionEvent(dpy, DefaultScreen(dpy), heads[0].x_org + 1,
		                     heads[0].y_org + heads[0].height - 1, CurrentTime);
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
		CHECK(open_switcher(dpy, root, XK_Control_L, &panel), "could not open for pointer crossing");
		XTestFakeMotionEvent(dpy, DefaultScreen(dpy), heads[1].x_org + 1,
		                     heads[1].y_org + heads[1].height - 1, CurrentTime);
		CHECK(wait_panel(dpy, root, 0, &panel) && wait_active(dpy, root, b2),
		      "pointer crossing did not cancel the switcher and select monitor B");
		switcher_key(dpy, XK_Control_L, False);
		CHECK(open_switcher(dpy, root, XK_Control_L, &panel) && panel_on(panel, heads[1]),
		      "reopening after pointer monitor selection did not stay on B");
		switcher_tap(dpy, XK_Escape);
		switcher_key(dpy, XK_Control_L, False);
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
