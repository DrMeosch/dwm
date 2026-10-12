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
	}
	XCloseDisplay(dpy);
	if (failures)
		return EXIT_FAILURE;
	puts("X11 input regression checks passed");
	return EXIT_SUCCESS;
}
