#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Display *dpy;
static Window win;
static GC gc;
static unsigned long appbg, card, ink, muted, rule;

static void
draw_text(int x, int y, const char *text, unsigned long color)
{
	XSetForeground(dpy, gc, color);
	XDrawString(dpy, win, gc, x, y, text, (int)strlen(text));
}

static unsigned long
named(const char *name)
{
	XColor screen, exact;
	if (!XAllocNamedColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), name, &screen, &exact)) {
		fprintf(stderr, "cannot allocate color %s\n", name);
		exit(EXIT_FAILURE);
	}
	return screen.pixel;
}

static void
draw(void)
{
	int width = 660, height = 420;
	XSetForeground(dpy, gc, appbg);
	XFillRectangle(dpy, win, gc, 0, 0, width, height);
	XSetForeground(dpy, gc, card);
	XFillRectangle(dpy, win, gc, 0, 0, width, 58);
	XSetForeground(dpy, gc, rule);
	XFillRectangle(dpy, win, gc, 0, 57, width, 1);
	draw_text(38, 37, "Xresources reload", ink);
	draw_text(38, 112,
	          "The selected window remains open while dwm refreshes its palette.", muted);
	XSetForeground(dpy, gc, rule);
	XFillRectangle(dpy, win, gc, 38, 145, 584, 1);
	draw_text(38, 187, "SIGUSR2  /  Xresources", ink);
	draw_text(38, 216, "Selected client  |  borderpx: 1 dark / 2 light", muted);
	XSetForeground(dpy, gc, rule);
	XFillRectangle(dpy, win, gc, 38, 367, 584, 1);
	draw_text(38, 394, "dwm issue #10  -  border width reload evidence", muted);
}

static int
client(const char *idfile)
{
	Window root;
	Window tagged;
	XClassHint class_hint = { "issue10", "Issue10Evidence" };
	XClassHint tagged_class = { "issue10", "Issue10Tagged" };
	XSizeHints size_hints;
	XEvent event;
	unsigned long tag_info[] = { 1UL << 1, 0 };
	FILE *file;

	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "cannot open X display\n");
		return EXIT_FAILURE;
	}
	root = DefaultRootWindow(dpy);
	appbg = named("#f0f1f6");
	card = named("#e5e7ee");
	ink = named("#282a36");
	muted = named("#555b6e");
	rule = named("#c7cad4");
	win = XCreateSimpleWindow(dpy, root, 270, 165, 660, 420, 0, ink, appbg);
	XStoreName(dpy, win, "Issue #10 | selected window");
	XSetClassHint(dpy, win, &class_hint);
	memset(&size_hints, 0, sizeof size_hints);
	size_hints.flags = PMinSize | PMaxSize;
	size_hints.min_width = size_hints.max_width = 660;
	size_hints.min_height = size_hints.max_height = 420;
	XSetWMNormalHints(dpy, win, &size_hints);
	tagged = XCreateSimpleWindow(dpy, root, 30, 30, 100, 100, 0, ink, appbg);
	XStoreName(dpy, tagged, "Issue #10 | retained tag");
	XSetClassHint(dpy, tagged, &tagged_class);
	XChangeProperty(dpy, tagged, XInternAtom(dpy, "_NET_CLIENT_INFO", False),
	                XA_CARDINAL, 32, PropModeReplace,
	                (unsigned char *)tag_info, 2);
	XSelectInput(dpy, win, ExposureMask | StructureNotifyMask);
	gc = XCreateGC(dpy, win, 0, NULL);
	XMapWindow(dpy, win);
	XMapWindow(dpy, tagged);
	XFlush(dpy);
	file = fopen(idfile, "w");
	if (!file) {
		perror(idfile);
		return EXIT_FAILURE;
	}
	fprintf(file, "%lu %lu\n", win, tagged);
	fclose(file);
	while (1) {
		while (XPending(dpy)) {
			XNextEvent(dpy, &event);
			if (event.type == Expose && event.xexpose.count == 0)
				draw();
		}
		usleep(10000);
	}
}

static int
check(const char *idarg, const char *expected_name, const char *widtharg,
      const char *taggedarg)
{
	Window target, tagged, focus;
	XWindowAttributes wa, tagged_attributes;
	Atom actual, client_info, wm_state;
	int format;
	unsigned long items, after;
	unsigned char *data = NULL;
	XImage *frame;
	XColor expected, screen_color;
	int revert, x, y, colored = 0;
	int frame_x, frame_y;
	unsigned long id = strtoul(idarg, NULL, 10);
	int expected_width = (int)strtoul(widtharg, NULL, 10);
	unsigned long tagged_id = strtoul(taggedarg, NULL, 10);

	dpy = XOpenDisplay(NULL);
	if (!dpy)
		return EXIT_FAILURE;
	target = (Window)id;
	if (!XGetWindowAttributes(dpy, target, &wa)) {
		fprintf(stderr, "window %lu does not exist\n", id);
		return EXIT_FAILURE;
	}
	focus = None;
	XGetInputFocus(dpy, &focus, &revert);
	expected = (XColor){0};
	if (!XAllocNamedColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)),
	                      expected_name, &screen_color, &expected))
		return EXIT_FAILURE;
	frame_x = wa.x - 2;
	frame_y = wa.y - 2;
	frame = XGetImage(dpy, DefaultRootWindow(dpy), frame_x, frame_y,
	                  (unsigned int)wa.width + 4, (unsigned int)wa.height + 4,
	                  AllPlanes, ZPixmap);
	if (!frame)
		return EXIT_FAILURE;
	for (y = 0; y < frame->height; y++)
		for (x = 0; x < frame->width; x++)
			if ((x < 4 || x >= frame->width - 4 || y < 4 || y >= frame->height - 4)
			&& XGetPixel(frame, x, y) == expected.pixel)
				colored++;
	printf("window=%lu geometry=%dx%d+%d+%d borderpx=%d focus=%s border-color-pixels=%d\n",
	       id, wa.width, wa.height, wa.x, wa.y, wa.border_width,
	       focus == target ? "selected" : "other", colored);
	if (wa.border_width != expected_width || focus != target
	|| colored < (wa.width + wa.height) / 2)
		return EXIT_FAILURE;
	tagged = (Window)tagged_id;
	client_info = XInternAtom(dpy, "_NET_CLIENT_INFO", False);
	wm_state = XInternAtom(dpy, "WM_STATE", False);
	if (!XGetWindowAttributes(dpy, tagged, &tagged_attributes)
	|| tagged_attributes.x + tagged_attributes.width
	   + 2 * tagged_attributes.border_width > 0
	|| tagged_attributes.border_width != expected_width
	|| XGetWindowProperty(dpy, tagged, wm_state, 0, 2, False, wm_state,
	                     &actual, &format, &items, &after, &data) != Success
	|| actual != wm_state || format != 32 || items < 1) {
		fprintf(stderr, "tagged client is not managed and hidden offscreen on its retained tag\n");
		if (data)
			XFree(data);
		return EXIT_FAILURE;
	}
	XFree(data);
	data = NULL;
	if (XGetWindowProperty(dpy, tagged, client_info, 0, 2, False, XA_CARDINAL,
	                       &actual, &format, &items, &after, &data) != Success
	|| actual != XA_CARDINAL || format != 32 || items != 2
	|| ((unsigned long *)data)[0] != (1UL << 1)) {
		fprintf(stderr, "dwm did not retain the tagged client's tag assignment\n");
		if (data)
			XFree(data);
		return EXIT_FAILURE;
	}
	XFree(data);
	XDestroyImage(frame);
	return EXIT_SUCCESS;
}

static unsigned char
channel(unsigned long pixel, unsigned long mask)
{
	unsigned int shift = 0;
	unsigned long value;
	if (!mask)
		return 0;
	while (!(mask & 1)) {
		mask >>= 1;
		shift++;
	}
	value = (pixel >> shift) & mask;
	return (unsigned char)((value * 255 + mask / 2) / mask);
}

static int
capture(const char *path)
{
	Window root;
	XImage *image;
	FILE *file;
	int width, height, x, y;

	dpy = XOpenDisplay(NULL);
	if (!dpy)
		return EXIT_FAILURE;
	root = DefaultRootWindow(dpy);
	width = DisplayWidth(dpy, DefaultScreen(dpy));
	height = DisplayHeight(dpy, DefaultScreen(dpy));
	XSync(dpy, False);
	image = XGetImage(dpy, root, 0, 0, (unsigned int)width, (unsigned int)height,
	                  AllPlanes, ZPixmap);
	if (!image) {
		fprintf(stderr, "cannot capture X root window\n");
		return EXIT_FAILURE;
	}
	file = fopen(path, "wb");
	if (!file) {
		perror(path);
		return EXIT_FAILURE;
	}
	fprintf(file, "P6\n%d %d\n255\n", width, height);
	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++) {
			unsigned long pixel = XGetPixel(image, x, y);
			fputc(channel(pixel, image->red_mask), file);
			fputc(channel(pixel, image->green_mask), file);
			fputc(channel(pixel, image->blue_mask), file);
		}
	XDestroyImage(image);
	XCloseDisplay(dpy);
	fclose(file);
	return EXIT_SUCCESS;
}

static int
rootcolor(const char *name)
{
	Window root;
	unsigned long pixel;
	dpy = XOpenDisplay(NULL);
	if (!dpy)
		return EXIT_FAILURE;
	root = DefaultRootWindow(dpy);
	pixel = named(name);
	XSetWindowBackground(dpy, root, pixel);
	XClearWindow(dpy, root);
	XSync(dpy, False);
	XCloseDisplay(dpy);
	return EXIT_SUCCESS;
}

int
main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "client"))
		return client(argv[2]);
	if (argc == 6 && !strcmp(argv[1], "check"))
		return check(argv[2], argv[3], argv[4], argv[5]);
	if (argc == 3 && !strcmp(argv[1], "capture"))
		return capture(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "root"))
		return rootcolor(argv[2]);
	fprintf(stderr, "usage: %s client idfile | check window-id color border-width tagged-window-id | capture ppm-file | root color\n", argv[0]);
	return EXIT_FAILURE;
}
