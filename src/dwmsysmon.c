/* See LICENSE file for copyright and license details. */
/*
 * dwmsysmon: samples system load outside the window manager and publishes the
 * latest value in the root window property read by dwm (see sysmon.h). It only
 * ever writes one short property, never blocks dwm, and exits with the X
 * connection. While dwm shows no system monitor it sleeps without a timer.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include "sysmon.h"

#ifndef SYSMON_STAT_PATH
#define SYSMON_STAT_PATH "/proc/stat"
#endif
#define INTERVAL 2000 /* ms between samples */
#define FIRST_TICK 500 /* ms until the first value after sampling starts */

static int sigfd[2];
static Display *dpy;
static Window root;
static Atom sysmonatom, enabledatom;

static void
onsignal(int sig)
{
	int saved = errno;
	ssize_t r;

	(void)sig;
	r = write(sigfd[1], "x", 1);
	(void)r;
	errno = saved;
}

static long long
nowms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Read the aggregate CPU counters; -1 if the file is missing, unreadable or malformed. */
static int
readcpu(const char *path, SysmonCpu *cpu)
{
	char buf[512];
	ssize_t n;
	int fd;

	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
		return -1;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	/* a first line that does not fit would be cut mid-number */
	if (!strchr(buf, '\n') && (size_t)n == sizeof buf - 1)
		return -1;
	return sysmon_cpu_parse(buf, cpu);
}

/* dwm clears the flag while the section is hidden; no flag means it is wanted. */
static int
enabled(void)
{
	Atom type;
	int format, on = 1;
	unsigned long n, after;
	unsigned char *data = NULL;

	if (XGetWindowProperty(dpy, root, enabledatom, 0, 1, False, XA_CARDINAL,
	    &type, &format, &n, &after, &data) == Success && data) {
		if (type == XA_CARDINAL && format == 32 && n == 1)
			on = *(unsigned long *)data != 0;
		XFree(data);
	}
	return on;
}

static void
publish(int cpu)
{
	char buf[16];
	int n;

	if ((n = sysmon_encode(buf, sizeof buf, cpu)) < 0)
		XDeleteProperty(dpy, root, sysmonatom);
	else
		XChangeProperty(dpy, root, sysmonatom, XA_STRING, 8, PropModeReplace,
		    (unsigned char *)buf, n);
	XFlush(dpy);
}

static void
usage(void)
{
	fputs("usage: dwmsysmon\n", stderr);
	exit(2);
}

int
main(int argc, char *argv[])
{
	struct sigaction sa;
	struct pollfd fds[2];
	SysmonCpu prev, cur;
	XEvent ev;
	long long next = 0, t;
	int on, have = 0, timeout;

	(void)argv;
	if (argc != 1)
		usage();
	if (!(dpy = XOpenDisplay(NULL))) {
		fputs("dwmsysmon: cannot open display\n", stderr);
		return 1;
	}
	root = DefaultRootWindow(dpy);
	sysmonatom = XInternAtom(dpy, SYSMON_ATOM, False);
	enabledatom = XInternAtom(dpy, SYSMON_ENABLED_ATOM, False);
	XSelectInput(dpy, root, PropertyChangeMask);

	if (pipe(sigfd) < 0 || fcntl(sigfd[1], F_SETFL, O_NONBLOCK) < 0) {
		perror("dwmsysmon: pipe");
		return 1;
	}
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = onsignal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	fds[0].fd = ConnectionNumber(dpy);
	fds[1].fd = sigfd[0];
	fds[0].events = fds[1].events = POLLIN;

	on = enabled();
	if (on) {
		have = readcpu(SYSMON_STAT_PATH, &prev) == 0;
		next = nowms() + FIRST_TICK;
	}
	for (;;) {
		while (XPending(dpy)) {
			XNextEvent(dpy, &ev);
			if (ev.type != PropertyNotify || ev.xproperty.window != root
			|| ev.xproperty.atom != enabledatom || enabled() == on)
				continue;
			on = !on;
			have = 0;
			if (on) {
				have = readcpu(SYSMON_STAT_PATH, &prev) == 0;
				next = nowms() + FIRST_TICK;
			} else
				publish(-1);
		}
		timeout = -1;
		if (on) {
			if ((t = nowms()) >= next) {
				if (readcpu(SYSMON_STAT_PATH, &cur) == 0) {
					publish(have ? sysmon_cpu_percent(&prev, &cur) : -1);
					prev = cur;
					have = 1;
				} else {
					publish(-1);
					have = 0;
				}
				next = t + INTERVAL;
			}
			timeout = (int)(next - nowms());
			if (timeout < 0)
				timeout = 0;
		}
		if (poll(fds, 2, timeout) < 0 && errno != EINTR)
			break;
		if (fds[1].revents || (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)))
			break;
	}
	XDeleteProperty(dpy, root, sysmonatom);
	XCloseDisplay(dpy);
	return 0;
}
