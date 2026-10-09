#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include "sigusr2.h"

static int sigusr2pipe[2] = { -1, -1 };
static int initialized;
static struct sigaction previous_action;
static volatile sig_atomic_t reload_requested;

static void
sigusr2_handler(int signal_number)
{
	int saved_errno = errno;
	char byte = 0;

	(void)signal_number;
	reload_requested = 1;
	if (sigusr2pipe[1] >= 0)
		(void)write(sigusr2pipe[1], &byte, sizeof byte);
	errno = saved_errno;
}

static int
set_nonblocking_cloexec(int fd)
{
	int flags;

	if ((flags = fcntl(fd, F_GETFL)) < 0
	|| fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0
	|| (flags = fcntl(fd, F_GETFD)) < 0
	|| fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
		return -1;
	return 0;
}

int
sigusr2_setup(void)
{
	struct sigaction action = {0};
	int saved_errno;

	if (initialized) {
		errno = EALREADY;
		return -1;
	}
	if (pipe(sigusr2pipe) < 0)
		return -1;
	if (set_nonblocking_cloexec(sigusr2pipe[0]) < 0
	|| set_nonblocking_cloexec(sigusr2pipe[1]) < 0)
		goto fail;

	action.sa_handler = sigusr2_handler;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGUSR2, &action, &previous_action) < 0)
		goto fail;
	initialized = 1;
	return 0;

fail:
	saved_errno = errno;
	close(sigusr2pipe[0]);
	close(sigusr2pipe[1]);
	sigusr2pipe[0] = sigusr2pipe[1] = -1;
	errno = saved_errno;
	return -1;
}

int
sigusr2_fd(void)
{
	return sigusr2pipe[0];
}

int
sigusr2_dispatch(void (*reload)(void))
{
	char bytes[128];
	int pending, saved_errno;
	ssize_t n;
	sigset_t block, previous;

	if (!initialized || !reload) {
		errno = EINVAL;
		return -1;
	}
	if (!reload_requested)
		return 0;
	sigemptyset(&block);
	sigaddset(&block, SIGUSR2);
	if (sigprocmask(SIG_BLOCK, &block, &previous) < 0)
		return -1;
	pending = reload_requested != 0;
	reload_requested = 0;
	do {
		n = read(sigusr2pipe[0], bytes, sizeof bytes);
	} while (n > 0 || (n < 0 && errno == EINTR));
	if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		saved_errno = errno;
		sigprocmask(SIG_SETMASK, &previous, NULL);
		errno = saved_errno;
		return -1;
	}
	if (sigprocmask(SIG_SETMASK, &previous, NULL) < 0)
		return -1;
	if (pending)
		reload();
	return pending;
}

void
sigusr2_cleanup(void)
{
	sigset_t block, previous;

	if (!initialized)
		return;
	sigemptyset(&block);
	sigaddset(&block, SIGUSR2);
	if (sigprocmask(SIG_BLOCK, &block, &previous) < 0)
		return;
	sigaction(SIGUSR2, &previous_action, NULL);
	close(sigusr2pipe[0]);
	close(sigusr2pipe[1]);
	sigusr2pipe[0] = sigusr2pipe[1] = -1;
	initialized = 0;
	sigprocmask(SIG_SETMASK, &previous, NULL);
}
