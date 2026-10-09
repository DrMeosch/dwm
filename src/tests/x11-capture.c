#include <X11/Xlib.h>
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
	draw_text(38, 216, "Selected client  |  borderpx: 2", muted);
	XSetForeground(dpy, gc, rule);
	XFillRectangle(dpy, win, gc, 38, 367, 584, 1);
	draw_text(38, 394, "dwm issue #6  -  visual reload evidence", muted);
}

static int
client(const char *idfile)
{
	Window root;
	XClassHint class_hint = { "issue6", "Issue6Evidence" };
	XSizeHints size_hints;
	XEvent event;
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
	XStoreName(dpy, win, "Issue #6 | selected window");
	XSetClassHint(dpy, win, &class_hint);
	memset(&size_hints, 0, sizeof size_hints);
	size_hints.flags = PMinSize | PMaxSize;
	size_hints.min_width = size_hints.max_width = 660;
	size_hints.min_height = size_hints.max_height = 420;
	XSetWMNormalHints(dpy, win, &size_hints);
	XSelectInput(dpy, win, ExposureMask | StructureNotifyMask);
	gc = XCreateGC(dpy, win, 0, NULL);
	XMapWindow(dpy, win);
	XFlush(dpy);
	file = fopen(idfile, "w");
	if (!file) {
		perror(idfile);
		return EXIT_FAILURE;
	}
	fprintf(file, "%lu\n", win);
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
check(const char *idarg, const char *expected_name)
{
	Window target, focus;
	XWindowAttributes wa;
	XImage *frame;
	XColor expected, screen_color;
	int revert, x, y, colored = 0;
	int frame_x, frame_y;
	unsigned long id = strtoul(idarg, NULL, 10);

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
	if (wa.border_width != 1 || focus != target
	|| colored < (wa.width + wa.height) / 2)
		return EXIT_FAILURE;
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
	if (argc == 4 && !strcmp(argv[1], "check"))
		return check(argv[2], argv[3]);
	if (argc == 3 && !strcmp(argv[1], "capture"))
		return capture(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "root"))
		return rootcolor(argv[2]);
	fprintf(stderr, "usage: %s client idfile | check window-id color | capture ppm-file | root color\n", argv[0]);
	return EXIT_FAILURE;
}
