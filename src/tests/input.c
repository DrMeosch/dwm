#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>

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
wait_for_input(Display *dpy, Window win, int type, unsigned int detail)
{
	int elapsed;

	for (elapsed = 0; elapsed < 1000; elapsed += 10) {
		XEvent event;
		while (XPending(dpy)) {
			XNextEvent(dpy, &event);
			if (event.xany.window == win && event.type == type
			&& ((type == ButtonPress && event.xbutton.button == detail)
			 || (type == KeyPress && event.xkey.keycode == detail)))
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
	got_click = wait_for_input(dpy, unfocused, ButtonPress, Button1);
	CHECK(got_click, "click on an unfocused client was not delivered");
	if (got_click) {
		XTestFakeKeyEvent(dpy, acode, True, CurrentTime);
		XTestFakeKeyEvent(dpy, acode, False, CurrentTime);
		XFlush(dpy);
		got_key = wait_for_input(dpy, unfocused, KeyPress, acode);
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

int
main(void)
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
	supercode = XKeysymToKeycode(dpy, XK_Super_L);
	qcode = XKeysymToKeycode(dpy, XK_q);
	jcode = XKeysymToKeycode(dpy, XK_j);
	acode = XKeysymToKeycode(dpy, XK_a);
	CHECK(supercode != 0 && qcode != 0 && jcode != 0 && acode != 0,
	      "X server keymap is missing a required test keysym");
	if (supercode && qcode && jcode && acode) {
		test_idle_close(dpy, root, supercode, qcode);
		test_click_replay(dpy, root, DefaultScreen(dpy), supercode, jcode, acode);
		test_pertag_views(dpy, root);
	}
	XCloseDisplay(dpy);
	if (failures)
		return EXIT_FAILURE;
	puts("X11 input regression checks passed");
	return EXIT_SUCCESS;
}
