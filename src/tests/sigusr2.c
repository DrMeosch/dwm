#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../sigusr2.h"

static int failures;
static int reload_count;
static int callback_outside_handler;
static const char *resource_value = "old accent";
static char active_color[32] = "old accent";

#define CHECK(condition, ...) do { \
	if (!(condition)) { \
		fprintf(stderr, "FAIL: "); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
		failures++; \
	} \
} while (0)

static void
reload_resources(void)
{
	sigset_t current;

	reload_count++;
	callback_outside_handler = sigprocmask(SIG_SETMASK, NULL, &current) == 0
		&& !sigismember(&current, SIGUSR2);
	snprintf(active_color, sizeof active_color, "%s", resource_value);
}

int
main(void)
{
	struct pollfd fd;
	struct timespec delay = { .tv_sec = 0, .tv_nsec = 50000000 };
	pid_t child;
	int ready, status;

	if (sigusr2_setup() < 0) {
		perror("sigusr2_setup");
		return 1;
	}
	fd.fd = sigusr2_fd();
	fd.events = POLLIN;
	fd.revents = 0;
	ready = poll(&fd, 1, 0);
	CHECK(ready == 0, "reload pipe was readable before a signal");

	resource_value = "new accent";
	child = fork();
	if (child < 0) {
		perror("fork");
		sigusr2_cleanup();
		return 1;
	}
	if (child == 0) {
		while (nanosleep(&delay, &delay) < 0 && errno == EINTR);
		if (kill(getppid(), SIGUSR2) < 0)
			_exit(1);
		_exit(0);
	}

	do {
		ready = poll(&fd, 1, 1000);
	} while (ready < 0 && errno == EINTR);
	CHECK(ready == 1 && (fd.revents & POLLIN),
	      "SIGUSR2 did not wake the blocked event loop poll");
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status)
	      && WEXITSTATUS(status) == 0, "signal sender did not exit cleanly");
	CHECK(sigusr2_dispatch(reload_resources) == 1,
	      "pending SIGUSR2 did not dispatch the reload callback");
	CHECK(reload_count == 1, "reload callback ran %d times, expected once", reload_count);
	CHECK(callback_outside_handler,
	      "reload callback ran with SIGUSR2 blocked as it is in its handler");
	CHECK(strcmp(active_color, "new accent") == 0,
	      "reload callback did not apply the updated resource value");
	CHECK(poll(&fd, 1, 0) == 0, "reload pipe was not drained after dispatch");
	CHECK(sigusr2_dispatch(reload_resources) == 0,
	      "an already handled signal triggered a second reload");
	CHECK(reload_count == 1, "reload callback ran again without a new signal");

	sigusr2_cleanup();
	if (failures)
		return 1;
	puts("SIGUSR2 reload behavioral checks passed");
	return 0;
}
