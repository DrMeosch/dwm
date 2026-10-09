/* Headless checks for allocation and fatal-error utility behavior. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../util.c"

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
test_ecalloc(void)
{
	unsigned char *memory = ecalloc(32, sizeof(*memory));
	unsigned int i;

	for (i = 0; i < 32; i++)
		CHECK(memory[i] == 0, "ecalloc returned non-zero byte %u", i);
	free(memory);
}

static void
check_die(const char *format, int expected_errno, const char *needle)
{
	int pipefd[2];
	char output[256] = {0};
	pid_t child;
	ssize_t n, total = 0;
	int status;

	CHECK(pipe(pipefd) == 0, "pipe failed while testing die");
	if (failures)
		return;
	child = fork();
	CHECK(child >= 0, "fork failed while testing die");
	if (child == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		errno = expected_errno;
		die(format);
		_exit(99);
	}
	close(pipefd[1]);
	while (total < (ssize_t)sizeof(output) - 1
	       && (n = read(pipefd[0], output + total,
                     sizeof(output) - 1 - (size_t)total)) > 0)
		total += n;
	output[total] = '\0';
	close(pipefd[0]);
	waitpid(child, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1,
	      "die exited with an unexpected status");
	CHECK(total > 0 && strstr(output, needle) != NULL,
	      "die output did not contain '%s': %s", needle, output);
}

int
main(void)
{
	test_ecalloc();
	check_die("fatal", 0, "fatal\n");
	check_die("open:", ENOENT, "open: ");
	if (failures)
		return EXIT_FAILURE;
	puts("utility regression checks passed");
	return EXIT_SUCCESS;
}
