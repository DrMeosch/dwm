/* Checks that run() dispatches events Xlib left queued in XCB before it blocks. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define main dwm_program_main
#include "../dwm.c"
#undef main

static Display *client;
static int maprequests;

static void
mapclient(void)
{
	Window w = XCreateSimpleWindow(client, DefaultRootWindow(client),
		0, 0, 10, 10, 0, 0, 0);

	XMapWindow(client, w);
	XSync(client, False);
}

/* Like manage() calling winpid(): a raw XCB round trip inside a handler
 * reads the next MapRequest into XCB's queue, where poll cannot see it. */
static void
maprequestroundtrip(XEvent *e)
{
	(void)e;
	if (++maprequests == 1) {
		mapclient();
		free(xcb_get_input_focus_reply(xcon, xcb_get_input_focus(xcon), NULL));
	} else
		running = 0;
}

static int
eventloop(void)
{
	if (!(dpy = XOpenDisplay(NULL)) || !(client = XOpenDisplay(NULL)))
		die("eventloop: cannot open display");
	if (!(xcon = XGetXCBConnection(dpy)))
		die("eventloop: cannot get xcb connection");
	if (sigusr2_setup() < 0)
		die("eventloop: sigusr2 setup:");
	XSelectInput(dpy, DefaultRootWindow(dpy), SubstructureRedirectMask);
	XSync(dpy, False);
	handler[MapRequest] = maprequestroundtrip;
	mapclient();
	alarm(2);
	run();
	return maprequests == 2 ? 0 : 1;
}

int
main(void)
{
	int status;
	pid_t pid;

	if ((pid = fork()) < 0)
		die("eventloop: fork:");
	if (pid == 0)
		_exit(eventloop());
	if (waitpid(pid, &status, 0) < 0)
		die("eventloop: waitpid:");
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "FAIL: event loop blocked with a MapRequest queued in XCB\n");
		return 1;
	}
	puts("event loop regression check passed");
	return 0;
}
